package eventstore

import (
	"testing"
)

func TestMarshalUnmarshalJSON(t *testing.T) {
	e := &Envelope{
		EventID:       "evt-1",
		AggregateID:   "agg-1",
		AggregateType: "InstrumentOrderBook",
		BenchmarkID:   "bm-1",
		EventType:     "OrderPlaced",
		Version:       1,
		TimestampNs:   123456,
		Metadata:      map[string]string{"producer": "test"},
		Payload:       []byte("hello"),
	}

	b, err := marshalEnvelope(e)
	if err != nil {
		t.Fatalf("marshal failed: %v", err)
	}
	var out Envelope
	if err := unmarshalEnvelope(b, &out); err != nil {
		t.Fatalf("unmarshal failed: %v", err)
	}
	if out.EventID != e.EventID || out.AggregateID != e.AggregateID || out.EventType != e.EventType {
		t.Fatalf("mismatch after roundtrip: got %+v want %+v", out, e)
	}
	if string(out.Payload) != string(e.Payload) {
		t.Fatalf("payload mismatch: %q vs %q", out.Payload, e.Payload)
	}
}

func TestGetLatestVersionAndNoBrokersAppend(t *testing.T) {
	s := NewRedpandaEventStore(nil)

	v, err := s.GetLatestVersion("missing")
	if err != nil {
		t.Fatalf("GetLatestVersion error: %v", err)
	}
	if v != -1 {
		t.Fatalf("expected -1 for missing aggregate, got %d", v)
	}

	// Append an event with no brokers configured (in-memory path)
	e := &Envelope{AggregateID: "agg-A", Version: 1, Metadata: map[string]string{"command_id": "cmd-1"}}
	if err := s.AppendEvents("events", []*Envelope{e}, nil); err != nil {
		t.Fatalf("AppendEvents failed: %v", err)
	}
	v2, _ := s.GetLatestVersion("agg-A")
	if v2 != 1 {
		t.Fatalf("expected latest version 1, got %d", v2)
	}

	// Duplicate command should be idempotent (skipped)
	dup := &Envelope{AggregateID: "agg-A", Version: 1, Metadata: map[string]string{"command_id": "cmd-1"}}
	if err := s.AppendEvents("events", []*Envelope{dup}, nil); err != nil {
		t.Fatalf("AppendEvents failed on duplicate: %v", err)
	}
	v3, _ := s.GetLatestVersion("agg-A")
	if v3 != 1 {
		t.Fatalf("expected latest version still 1 after duplicate, got %d", v3)
	}
}

func TestAppendEvents_ExpectedVersionMismatch(t *testing.T) {
	s := NewRedpandaEventStore(nil)
	// Expect version 0 but current is -1 -> should error
	ev := &Envelope{AggregateID: "agg-B", Version: 1}
	evExp := int64(0)
	if err := s.AppendEvents("events", []*Envelope{ev}, &evExp); err == nil {
		t.Fatalf("expected concurrency error but append succeeded")
	}
}
