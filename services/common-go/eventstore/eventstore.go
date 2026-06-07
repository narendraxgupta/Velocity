package eventstore

// Lightweight envelope used by the Go-side event store API.
// This mirrors the Protobuf `EventEnvelope` in `proto/events.proto` but
// keeps the runtime dependency small until codegen is wired.
type Envelope struct {
	EventID       string            `json:"event_id"`
	AggregateID   string            `json:"aggregate_id"`
	AggregateType string            `json:"aggregate_type"`
	BenchmarkID   string            `json:"benchmark_id"`
	EventType     string            `json:"event_type"`
	Version       int64             `json:"version"`
	TimestampNs   int64             `json:"timestamp_ns"`
	Metadata      map[string]string `json:"metadata"`
	Payload       []byte            `json:"payload"` // opaque proto/json payload
}

// IEventStore is the abstract event store interface used by control-plane
// services. Implementations wrap Redpanda/Kafka and provide optimistic
// concurrency, idempotency checks, and snapshot hooks.
type IEventStore interface {
	// AppendEvents appends an ordered slice of events to the given stream/topic.
	// If expectedVersion != nil the append is conditional on the stream's
	// current version matching expectedVersion (optimistic concurrency).
	AppendEvents(stream string, events []*Envelope, expectedVersion *int64) error

	// LoadStream returns events from a stream starting at `fromOffset` (inclusive).
	// A consumer can use this to page through historical events.
	LoadStream(stream string, fromOffset int64, limit int) ([]*Envelope, error)

	// LoadAggregate returns events for a single aggregate id starting at a
	// version (useful for reconstructing an aggregate's state).
	LoadAggregate(aggregateID string, fromVersion int64) ([]*Envelope, error)

	// Replay consumes the stream from the beginning (or a configured offset)
	// and passes events to the handler in order. Handler errors stop the replay.
	Replay(stream string, handler func(*Envelope) error) error

	// Snapshot stores a snapshot pointer for an aggregate (blob URL). Implementations
	// should make snapshots discoverable for fast bootstrapping of replayers.
	Snapshot(aggregateID string, version int64, snapshotBlobURL string) error

	// GetLatestVersion returns the latest persisted version for an aggregate,
	// or -1 if none exists.
	GetLatestVersion(aggregateID string) (int64, error)
}
