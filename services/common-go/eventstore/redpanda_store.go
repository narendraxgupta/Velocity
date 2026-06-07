package eventstore

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
	"time"

	kafka "github.com/segmentio/kafka-go"
)

// NOTE: This file contains a skeleton implementation. The production
// implementation should use a high-performance Kafka client (franz-go,
// confluent-kafka-go or segmentio/kafka-go) and a Schema Registry integration.

// RedpandaEventStore is a minimal placeholder implementation that demonstrates
// how the `IEventStore` methods will be wired. It intentionally leaves
// low-level client wiring for a follow-up change.
type RedpandaEventStore struct {
	// brokers, topic configuration, serializers, and other dependencies
	Brokers      []string
	mu           sync.Mutex
	versions     map[string]int64 // aggregateID -> latest version
	seenCommands map[string]int64 // command_id -> version (simple dedupe)
}

func NewRedpandaEventStore(brokers []string) *RedpandaEventStore {
	return &RedpandaEventStore{
		Brokers:      brokers,
		versions:     make(map[string]int64),
		seenCommands: make(map[string]int64),
	}
}

func (r *RedpandaEventStore) AppendEvents(stream string, events []*Envelope, expectedVersion *int64) error {
	if len(events) == 0 {
		return errors.New("no events to append")
	}

	// Shortcut for test environments: if no brokers configured, don't
	// attempt network IO — perform in-memory optimistic checks and index
	// updates so unit tests can exercise concurrency/dedupe logic.
	if len(r.Brokers) == 0 {
		r.mu.Lock()
		defer r.mu.Unlock()

		// Check expected version
		if expectedVersion != nil {
			cur := int64(-1)
			if v, ok := r.versions[events[0].AggregateID]; ok {
				cur = v
			}
			if cur != *expectedVersion {
				return fmt.Errorf("concurrency error: expected %d but current %d", *expectedVersion, cur)
			}
		}

		// Deduplicate and update in-memory indexes
		anyWritten := false
		for _, e := range events {
			cmd := ""
			if e.Metadata != nil {
				if v, ok := e.Metadata["command_id"]; ok {
					cmd = v
				}
			}
			if cmd != "" {
				if prev, ok := r.seenCommands[cmd]; ok && prev >= e.Version {
					// already applied this command (idempotent), skip
					continue
				}
				r.seenCommands[cmd] = e.Version
			}
			r.versions[e.AggregateID] = e.Version
			anyWritten = true
		}
		if !anyWritten {
			return nil
		}
		return nil
	}

	// Build kafka writer for the target stream (topic)
	w := kafka.NewWriter(kafka.WriterConfig{
		Brokers:  r.Brokers,
		Topic:    stream,
		Balancer: &kafka.Hash{},
	})
	defer w.Close()

	msgs := make([]kafka.Message, 0, len(events))
	for _, e := range events {
		b, err := marshalEnvelope(e)
		if err != nil {
			return fmt.Errorf("marshal envelope: %w", err)
		}
		// Key by aggregate for per-aggregate ordering
		msgs = append(msgs, kafka.Message{Key: []byte(e.AggregateID), Value: b})
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	// optimistic concurrency + idempotency (prototype in-memory)
	// Lock and check expectedVersion and simple dedupe before writing
	r.mu.Lock()
	defer r.mu.Unlock()

	// Check expected version against our in-memory index (best-effort)
	if expectedVersion != nil {
		cur := int64(-1)
		if v, ok := r.versions[events[0].AggregateID]; ok {
			cur = v
		}
		if cur != *expectedVersion {
			return fmt.Errorf("concurrency error: expected %d but current %d", *expectedVersion, cur)
		}
	}

	// Deduplicate by command_id in metadata when present
	filteredMsgs := make([]kafka.Message, 0, len(msgs))
	for i, e := range events {
		cmd := ""
		if e.Metadata != nil {
			if v, ok := e.Metadata["command_id"]; ok {
				cmd = v
			}
		}
		if cmd != "" {
			if prev, ok := r.seenCommands[cmd]; ok && prev >= e.Version {
				// already applied this command (idempotent), skip
				continue
			}
			// mark as seen (will be updated to event version after write)
			r.seenCommands[cmd] = e.Version
		}
		filteredMsgs = append(filteredMsgs, msgs[i])
	}

	if len(filteredMsgs) == 0 {
		// Nothing to write after dedupe
		return nil
	}

	if err := w.WriteMessages(ctx, filteredMsgs...); err != nil {
		return fmt.Errorf("kafka write: %w", err)
	}

	// Update in-memory latest version for the aggregate
	last := events[len(events)-1]
	r.versions[last.AggregateID] = last.Version
	return nil
}

func (r *RedpandaEventStore) LoadStream(stream string, fromOffset int64, limit int) ([]*Envelope, error) {
	// Basic implementation: read up to `limit` messages from the topic starting
	// at the earliest available offset. `fromOffset` is currently ignored and
	// should be implemented with partition/offset management in a production impl.
	if limit <= 0 {
		return nil, errors.New("limit must be > 0")
	}

	rdr := kafka.NewReader(kafka.ReaderConfig{
		Brokers: r.Brokers,
		Topic:   stream,
		GroupID: "eventstore-snapshot-reader",
	})
	defer rdr.Close()

	out := make([]*Envelope, 0, limit)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	for i := 0; i < limit; i++ {
		m, err := rdr.ReadMessage(ctx)
		if err != nil {
			// return what we have and the error
			if len(out) == 0 {
				return nil, fmt.Errorf("read message: %w", err)
			}
			return out, nil
		}
		var e Envelope
		if err := unmarshalEnvelope(m.Value, &e); err != nil {
			return nil, fmt.Errorf("unmarshal envelope: %w", err)
		}
		out = append(out, &e)
	}
	return out, nil
}

func (r *RedpandaEventStore) LoadAggregate(aggregateID string, fromVersion int64) ([]*Envelope, error) {
	// Prototype implementation: scan the topic and collect matching aggregate events.
	// This is inefficient for production but useful for testing and small streams.
	rdr := kafka.NewReader(kafka.ReaderConfig{
		Brokers: r.Brokers,
		Topic:   streamNameForAggregate(aggregateID),
		GroupID: "eventstore-aggregate-loader",
	})
	defer rdr.Close()

	out := make([]*Envelope, 0)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	for {
		m, err := rdr.ReadMessage(ctx)
		if err != nil {
			if len(out) == 0 {
				return nil, fmt.Errorf("read message: %w", err)
			}
			return out, nil
		}
		var e Envelope
		if err := unmarshalEnvelope(m.Value, &e); err != nil {
			return nil, fmt.Errorf("unmarshal envelope: %w", err)
		}
		if e.AggregateID != aggregateID {
			continue
		}
		if e.Version >= fromVersion {
			out = append(out, &e)
		}
	}
}

func (r *RedpandaEventStore) Replay(stream string, handler func(*Envelope) error) error {
	rdr := kafka.NewReader(kafka.ReaderConfig{
		Brokers: r.Brokers,
		Topic:   stream,
		GroupID: "eventstore-replayer",
	})
	defer rdr.Close()

	ctx := context.Background()
	for {
		m, err := rdr.ReadMessage(ctx)
		if err != nil {
			return fmt.Errorf("read message: %w", err)
		}
		var e Envelope
		if err := unmarshalEnvelope(m.Value, &e); err != nil {
			return fmt.Errorf("unmarshal envelope: %w", err)
		}
		if err := handler(&e); err != nil {
			return fmt.Errorf("handler error: %w", err)
		}
	}
}

// Default JSON-based marshalling. A build-tagged file may provide
// protobuf-backed implementations when `-tags proto` is used.
func marshalEnvelope(e *Envelope) ([]byte, error) {
	return json.Marshal(e)
}

func unmarshalEnvelope(b []byte, out *Envelope) error {
	return json.Unmarshal(b, out)
}

func (r *RedpandaEventStore) Snapshot(aggregateID string, version int64, snapshotBlobURL string) error {
	// Persist a pointer to the snapshot; in prod write a DB row in `snapshots`.
	// Placeholder: no-op.
	_ = aggregateID
	_ = version
	_ = snapshotBlobURL
	return nil
}

func (r *RedpandaEventStore) GetLatestVersion(aggregateID string) (int64, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if v, ok := r.versions[aggregateID]; ok {
		return v, nil
	}
	return -1, nil
}

func streamNameForAggregate(aggregateID string) string {
	// Prototype mapping: all aggregates live in the `events` topic in this repo.
	// Real implementations would encode partitioning/stream naming here.
	_ = aggregateID
	return "events"
}
