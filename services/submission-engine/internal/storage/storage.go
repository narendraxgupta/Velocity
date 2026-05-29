// Package storage is the thin abstraction over MinIO/S3.
//
// All artefact reads/writes funnel through this interface so we can swap
// to a different backend (memory, local-disk) in tests without changing
// call sites.
package storage

import (
	"context"
	"fmt"
	"io"
	"time"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

// PutOptions covers the subset of S3 metadata we actually use.
type PutOptions struct {
	ContentType string
	UserMeta    map[string]string // X-Amz-Meta-* headers
}

// Storage is the abstract interface every caller depends on.
type Storage interface {
	Put(ctx context.Context, key string, r io.Reader, size int64, opts PutOptions) (string, error)
	Get(ctx context.Context, key string) (io.ReadCloser, error)
	Stat(ctx context.Context, key string) (size int64, etag string, err error)
	PresignGet(ctx context.Context, key string, ttlSeconds uint32) (string, error)
}

// NewMinIO constructs a MinIO-backed Storage. The bucket is created on
// demand if it does not yet exist.
func NewMinIO(endpoint, accessKey, secretKey, bucket string, useTLS bool) (Storage, error) {
	cli, err := minio.New(endpoint, &minio.Options{
		Creds:  credentials.NewStaticV4(accessKey, secretKey, ""),
		Secure: useTLS,
	})
	if err != nil {
		return nil, fmt.Errorf("minio.New: %w", err)
	}

	// Ensure the bucket exists. Failure here is non-fatal — the user may
	// have provisioned it externally and lacks ListBuckets permission.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	exists, err := cli.BucketExists(ctx, bucket)
	if err == nil && !exists {
		_ = cli.MakeBucket(ctx, bucket, minio.MakeBucketOptions{})
	}

	return &minioStorage{cli: cli, bucket: bucket}, nil
}

type minioStorage struct {
	cli    *minio.Client
	bucket string
}

func (s *minioStorage) Put(ctx context.Context, key string, r io.Reader,
	size int64, opts PutOptions) (string, error) {
	putOpts := minio.PutObjectOptions{
		ContentType:  opts.ContentType,
		UserMetadata: opts.UserMeta,
	}
	if putOpts.ContentType == "" {
		putOpts.ContentType = "application/octet-stream"
	}
	if _, err := s.cli.PutObject(ctx, s.bucket, key, r, size, putOpts); err != nil {
		return "", fmt.Errorf("put %s: %w", key, err)
	}
	return fmt.Sprintf("s3://%s/%s", s.bucket, key), nil
}

func (s *minioStorage) Get(ctx context.Context, key string) (io.ReadCloser, error) {
	obj, err := s.cli.GetObject(ctx, s.bucket, key, minio.GetObjectOptions{})
	if err != nil {
		return nil, fmt.Errorf("get %s: %w", key, err)
	}
	return obj, nil
}

func (s *minioStorage) Stat(ctx context.Context, key string) (int64, string, error) {
	info, err := s.cli.StatObject(ctx, s.bucket, key, minio.StatObjectOptions{})
	if err != nil {
		return 0, "", fmt.Errorf("stat %s: %w", key, err)
	}
	return info.Size, info.ETag, nil
}

func (s *minioStorage) PresignGet(ctx context.Context, key string,
	ttlSeconds uint32) (string, error) {
	ttl := time.Duration(ttlSeconds) * time.Second
	if ttl == 0 {
		ttl = 15 * time.Minute
	}
	u, err := s.cli.PresignedGetObject(ctx, s.bucket, key, ttl, nil)
	if err != nil {
		return "", fmt.Errorf("presign %s: %w", key, err)
	}
	return u.String(), nil
}
