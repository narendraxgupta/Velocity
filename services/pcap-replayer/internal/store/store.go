// Package store wraps MinIO for the replayer.
//
// We only need Get + List (the recorder owns Put), so the surface is
// half the size of the recorder's. Keeping it separate from the engine's
// storage package lets us deploy the replayer with a different bucket
// (e.g. a customer's shared-pcap bucket) without dragging in unrelated
// dependencies.
package store

import (
	"context"
	"fmt"
	"io"
	"strings"
	"time"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

// PcapObject describes one entry in the bucket's `pcaps/` prefix.
type PcapObject struct {
	BenchmarkID  string    `json:"benchmark_id"`
	ObjectKey    string    `json:"object_key"`
	SizeBytes    int64     `json:"size_bytes"`
	LastModified time.Time `json:"last_modified_ns,omitempty"`
}

// Storage is the replayer's narrow MinIO surface.
type Storage interface {
	Get(ctx context.Context, key string) (io.ReadCloser, error)
	Stat(ctx context.Context, key string) (size int64, etag string, err error)
	ListPcaps(ctx context.Context, limit int) ([]PcapObject, error)
}

// NewMinIO connects to MinIO/S3.
func NewMinIO(endpoint, accessKey, secretKey, bucket string, useTLS bool) (Storage, error) {
	cli, err := minio.New(endpoint, &minio.Options{
		Creds:  credentials.NewStaticV4(accessKey, secretKey, ""),
		Secure: useTLS,
	})
	if err != nil {
		return nil, fmt.Errorf("minio.New: %w", err)
	}
	return &mio{cli: cli, bucket: bucket}, nil
}

type mio struct {
	cli    *minio.Client
	bucket string
}

func (s *mio) Get(ctx context.Context, key string) (io.ReadCloser, error) {
	obj, err := s.cli.GetObject(ctx, s.bucket, key, minio.GetObjectOptions{})
	if err != nil {
		return nil, fmt.Errorf("get %s: %w", key, err)
	}
	return obj, nil
}

func (s *mio) Stat(ctx context.Context, key string) (int64, string, error) {
	info, err := s.cli.StatObject(ctx, s.bucket, key, minio.StatObjectOptions{})
	if err != nil {
		return 0, "", err
	}
	return info.Size, info.ETag, nil
}

func (s *mio) ListPcaps(ctx context.Context, limit int) ([]PcapObject, error) {
	if limit <= 0 {
		limit = 50
	}
	// Wrap ctx so we can cancel the ListObjects worker as soon as we
	// reach `limit`. Without this, the minio-go client's internal
	// pump goroutine blocks forever sending into the unread channel
	// once we break the consumer loop. The leak is small per-request
	// but compounds quickly under traffic.
	listCtx, cancel := context.WithCancel(ctx)
	defer cancel()

	ch := s.cli.ListObjects(listCtx, s.bucket, minio.ListObjectsOptions{
		Prefix:    "pcaps/",
		Recursive: true,
	})
	out := make([]PcapObject, 0, limit)
	for info := range ch {
		if info.Err != nil {
			return nil, fmt.Errorf("list: %w", info.Err)
		}
		if !strings.HasSuffix(info.Key, ".pcap") {
			continue
		}
		benchID := strings.TrimSuffix(strings.TrimPrefix(info.Key, "pcaps/"), ".pcap")
		out = append(out, PcapObject{
			BenchmarkID:  benchID,
			ObjectKey:    info.Key,
			SizeBytes:    info.Size,
			LastModified: info.LastModified,
		})
		if len(out) >= limit {
			cancel()
			// Drain the channel so the producer goroutine sees the
			// cancellation and exits cleanly. Cheap because the
			// producer stops emitting almost immediately.
			for range ch {
			}
			break
		}
	}
	return out, nil
}
