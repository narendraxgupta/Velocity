// Package store persists critique results in Redis with a TTL.
//
// Critiques are large-ish blobs (a few KB JSON each) and we want them
// indexable both by (a) submission_id for the detail page and (b)
// critique_id for permalink / audit log entries. Redis hashes give us
// that for free without dragging in another data store.
//
// Key layout
// ----------
//
//	critique:{critique_id}                 → JSON blob (TTL 30d)
//	critique:by_submission:{submission_id} → critique_id (latest, TTL 30d)
//	critique:status:{critique_id}          → "pending"|"running"|"done"|"failed"
package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/redis/go-redis/v9"
)

const (
	// 30 days mirrors the gateway's submission retention default.
	defaultTTL = 30 * 24 * time.Hour
)

// Record is the persisted shape; it embeds the parsed critique plus
// some envelope fields used for status displays.
type Record struct {
	CritiqueID   string          `json:"critique_id"`
	SubmissionID string          `json:"submission_id"`
	Model        string          `json:"model"`
	CreatedAtMs  int64           `json:"created_at_ms"`
	LatencyMs    int64           `json:"latency_ms"`
	PromptTokens int             `json:"prompt_tokens,omitempty"`
	EvalTokens   int             `json:"eval_tokens,omitempty"`
	Critique     json.RawMessage `json:"critique"`
	// Raw text body is kept for forensic audits; clients should prefer
	// the parsed Critique field.
	Raw string `json:"raw,omitempty"`
}

// Status enumerates the lifecycle a critique passes through.
type Status string

const (
	StatusPending Status = "pending"
	StatusRunning Status = "running"
	StatusDone    Status = "done"
	StatusFailed  Status = "failed"
)

// Store wraps the redis client with our key conventions.
type Store struct {
	r   *redis.Client
	ttl time.Duration
}

// New constructs a Store using the default 30-day TTL.
func New(r *redis.Client) *Store { return &Store{r: r, ttl: defaultTTL} }

// SetStatus updates the status for a critique_id. Idempotent and cheap.
func (s *Store) SetStatus(ctx context.Context, critiqueID string, st Status) error {
	if s.r == nil {
		return errors.New("redis client not configured")
	}
	return s.r.Set(ctx, "critique:status:"+critiqueID, string(st), s.ttl).Err()
}

// GetStatus returns the lifecycle status, "" if missing.
func (s *Store) GetStatus(ctx context.Context, critiqueID string) (Status, error) {
	if s.r == nil {
		return "", nil
	}
	v, err := s.r.Get(ctx, "critique:status:"+critiqueID).Result()
	if errors.Is(err, redis.Nil) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	return Status(v), nil
}

// Save persists the record under its critique_id and indexes the
// submission → critique_id mapping. The mapping always points at the
// LATEST critique for a submission; older critiques are still
// fetchable by critique_id directly.
func (s *Store) Save(ctx context.Context, r Record) error {
	if s.r == nil {
		return errors.New("redis client not configured")
	}
	body, err := json.Marshal(r)
	if err != nil {
		return fmt.Errorf("marshal record: %w", err)
	}
	pipe := s.r.Pipeline()
	pipe.Set(ctx, "critique:"+r.CritiqueID, body, s.ttl)
	pipe.Set(ctx, "critique:by_submission:"+r.SubmissionID, r.CritiqueID, s.ttl)
	pipe.Set(ctx, "critique:status:"+r.CritiqueID, string(StatusDone), s.ttl)
	_, err = pipe.Exec(ctx)
	return err
}

// LookupBySubmission returns the latest critique for a submission, or
// nil if none has been produced yet.
func (s *Store) LookupBySubmission(ctx context.Context, submissionID string) (*Record, error) {
	if s.r == nil {
		return nil, nil
	}
	cid, err := s.r.Get(ctx, "critique:by_submission:"+submissionID).Result()
	if errors.Is(err, redis.Nil) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return s.Get(ctx, cid)
}

// Get returns a critique by ID, nil if missing.
func (s *Store) Get(ctx context.Context, critiqueID string) (*Record, error) {
	if s.r == nil {
		return nil, nil
	}
	v, err := s.r.Get(ctx, "critique:"+critiqueID).Result()
	if errors.Is(err, redis.Nil) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var out Record
	if err := json.Unmarshal([]byte(v), &out); err != nil {
		return nil, err
	}
	return &out, nil
}
