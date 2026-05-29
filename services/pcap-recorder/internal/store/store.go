// Package store is the recorder's narrow view of MinIO.
//
// We deliberately avoid pulling submission-engine's storage package — the
// recorder writes a single object kind (pcaps) and the dependency surface
// has to stay tight enough that this service can be deployed independently.
package store

import (
	"context"
	"fmt"
	"io"
	"time"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

// Storage is the recorder's persistence interface.
type Storage interface {
	// Put streams `r` (size unknown) into MinIO under key `key`. ContentType
	// is set to application/vnd.tcpdump.pcap for cooperating tooling
	// (Wireshark download intent, MIME-based hot-loaders, etc.).
	Put(ctx context.Context, key string, r io.Reader) (string, error)

	// Stat returns size/etag for a pcap object — used by GetPcapObjectKey
	// at the gateway to surface "available" vs "missing" to the user.
	Stat(ctx context.Context, key string) (size int64, etag string, err error)

	// PresignGet mints a short-lived download URL. We don't hand out
	// long-lived URLs because pcaps may contain captured FIX payloads
	// during a future production-replay use-case.
	PresignGet(ctx context.Context, key string, ttl time.Duration) (string, error)
}

// NewMinIO opens a connection to MinIO/S3 and ensures the bucket exists.
// Bucket creation failure is non-fatal: in cluster ops the bucket is
// pre-provisioned with quota, and we don't want recorder failures to mask
// that.
func NewMinIO(endpoint, accessKey, secretKey, bucket string, useTLS bool) (Storage, error) {
	cli, err := minio.New(endpoint, &minio.Options{
		Creds:  credentials.NewStaticV4(accessKey, secretKey, ""),
		Secure: useTLS,
	})
	if err != nil {
		return nil, fmt.Errorf("minio.New: %w", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if exists, _ := cli.BucketExists(ctx, bucket); !exists {
		_ = cli.MakeBucket(ctx, bucket, minio.MakeBucketOptions{})
	}
	return &mio{cli: cli, bucket: bucket}, nil
}

type mio struct {
	cli    *minio.Client
	bucket string
}

func (s *mio) Put(ctx context.Context, key string, r io.Reader) (string, error) {
	_, err := s.cli.PutObject(ctx, s.bucket, key, r, -1,
		minio.PutObjectOptions{
			ContentType: "application/vnd.tcpdump.pcap",
			UserMetadata: map[string]string{
				"recorder": "velocity-pcap-recorder",
			},
		})
	if err != nil {
		return "", fmt.Errorf("put %s: %w", key, err)
	}
	return fmt.Sprintf("s3://%s/%s", s.bucket, key), nil
}

func (s *mio) Stat(ctx context.Context, key string) (int64, string, error) {
	info, err := s.cli.StatObject(ctx, s.bucket, key, minio.StatObjectOptions{})
	if err != nil {
		return 0, "", err
	}
	return info.Size, info.ETag, nil
}

func (s *mio) PresignGet(ctx context.Context, key string, ttl time.Duration) (string, error) {
	if ttl <= 0 {
		ttl = 15 * time.Minute
	}
	u, err := s.cli.PresignedGetObject(ctx, s.bucket, key, ttl, nil)
	if err != nil {
		return "", err
	}
	return u.String(), nil
}
