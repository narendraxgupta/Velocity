// Package audit is the producer-side helper for Velocity's audit log.
// Every write-bearing API call should call audit.Emit once with the
// resolved tenant context, action verb, and outcome.
//
// Two delivery modes are supported:
//
//   - "kafka"  → fire-and-forget produce to `t.<tenant>.audit`.
//   - "stdout" → JSON line to stderr. Useful in dev where Kafka isn't
//                running, and as a fail-safe when Kafka is unreachable.
//
// We deliberately do NOT fail the originating request when audit
// emission fails — audit is a side-channel observation, not part of
// the request's correctness contract. We do bump a counter so the
// platform team is alerted if the rate of failures crosses an SLO.
package audit

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/oklog/ulid/v2"
	"github.com/twmb/franz-go/pkg/kgo"
)

// Action is a stable verb naming the operation, dotted by resource:
//   "submission.create" "benchmark.start" "chaos.inject"
// Keep this open so callers can add new actions without a code change
// here — but PLEASE keep them lowercase.
type Action string

// Outcome of the audited operation.
type Outcome string

const (
	OutcomeAllow Outcome = "allow"
	OutcomeDeny  Outcome = "deny"
	OutcomeError Outcome = "error"
)

// Event is the JSON shape we put on the wire. Must mirror
// services/audit-log/internal/event.Event — keep them in sync.
type Event struct {
	SchemaVersion int            `json:"v"`
	EventID       string         `json:"event_id"`
	TenantID      string         `json:"tenant_id"`
	Subject       string         `json:"subject"`
	Role          string         `json:"role"`
	SourceService string         `json:"source"`
	Action        Action         `json:"action"`
	ResourceType  string         `json:"resource_type"`
	ResourceID    string         `json:"resource_id"`
	Outcome       Outcome        `json:"outcome"`
	StatusCode    int            `json:"status_code"`
	OccurredAtNs  int64          `json:"occurred_at_ns"`
	RemoteIP      string         `json:"remote_ip"`
	RequestID     string         `json:"request_id"`
	Meta          map[string]any `json:"meta,omitempty"`
}

// Emitter sends Events. NOT goroutine-safe to swap underlying transport
// after construction, but Emit() is safe to call from many goroutines.
type Emitter interface {
	Emit(ctx context.Context, e Event)
	Close() error
}

// Config sets up an Emitter. ServiceName MUST be set; everything else
// defaults to sensible values.
type Config struct {
	ServiceName  string
	KafkaBrokers string // comma-separated; empty disables kafka
	FallbackOut  io.Writer
}

// Open returns the best Emitter we can build for the given config.
// Kafka if configured AND reachable, otherwise a writer that just
// prints to FallbackOut (defaulting to stderr).
func Open(cfg Config) Emitter {
	if cfg.ServiceName == "" {
		cfg.ServiceName = "unknown"
	}
	if cfg.FallbackOut == nil {
		cfg.FallbackOut = os.Stderr
	}
	if cfg.KafkaBrokers != "" {
		if cli, err := kgo.NewClient(
			kgo.SeedBrokers(strings.Split(cfg.KafkaBrokers, ",")...),
			kgo.AllowAutoTopicCreation(),
			kgo.ProducerLinger(50*time.Millisecond),
		); err == nil {
			return &kafkaEmitter{cli: cli, service: cfg.ServiceName, fallback: cfg.FallbackOut}
		}
	}
	return &stdoutEmitter{w: cfg.FallbackOut, service: cfg.ServiceName}
}

// ----------------------------------------------------------------------------
//  Kafka backend
// ----------------------------------------------------------------------------

type kafkaEmitter struct {
	cli      *kgo.Client
	service  string
	fallback io.Writer
	dropped  atomic.Uint64
}

func (k *kafkaEmitter) Emit(ctx context.Context, e Event) {
	enrich(&e, k.service)
	body, err := json.Marshal(&e)
	if err != nil {
		k.dropped.Add(1)
		return
	}
	rec := &kgo.Record{
		Topic: fmt.Sprintf("t.%s.audit", e.TenantID),
		Value: body,
		Key:   []byte(e.EventID),
	}
	// Async produce — we want zero blocking on the caller's hot path.
	// If Kafka is down we fall back to stderr so the event is at least
	// recoverable from container logs.
	k.cli.Produce(ctx, rec, func(_ *kgo.Record, err error) {
		if err != nil {
			k.dropped.Add(1)
			_, _ = fmt.Fprintf(k.fallback, "AUDIT_FALLBACK %s\n", body)
		}
	})
}

func (k *kafkaEmitter) Close() error {
	k.cli.Close()
	return nil
}

// ----------------------------------------------------------------------------
//  Stdout backend (dev / fallback)
// ----------------------------------------------------------------------------

type stdoutEmitter struct {
	w       io.Writer
	service string
	mu      sync.Mutex
}

func (s *stdoutEmitter) Emit(_ context.Context, e Event) {
	enrich(&e, s.service)
	body, err := json.Marshal(&e)
	if err != nil {
		return
	}
	s.mu.Lock()
	_, _ = s.w.Write(append(body, '\n'))
	s.mu.Unlock()
}

func (s *stdoutEmitter) Close() error { return nil }

// ----------------------------------------------------------------------------
//  Shared helpers
// ----------------------------------------------------------------------------

func enrich(e *Event, service string) {
	if e.EventID == "" {
		e.EventID = ulid.Make().String()
	}
	if e.OccurredAtNs == 0 {
		e.OccurredAtNs = time.Now().UnixNano()
	}
	if e.SourceService == "" {
		e.SourceService = service
	}
	if e.SchemaVersion == 0 {
		e.SchemaVersion = 1
	}
	if e.TenantID == "" {
		// Audit events MUST be tenant-scoped — but rather than dropping
		// the event we tag it as "unknown" so it still appears in the
		// chain. Operators triage by grep'ing `tenant_id=unknown` in
		// the audit table.
		e.TenantID = "unknown"
	}
}
