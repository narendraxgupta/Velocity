//go:build proto

package eventstore

import (
	"encoding/base64"
	"fmt"

	pb "github.com/velocity/proto"
	"google.golang.org/protobuf/proto"
)

// When built with `-tags proto` this file provides protobuf-backed
// serialization for Envelope. It places the opaque `Payload` into
// metadata as base64 to preserve the runtime-opaque bytes.
func marshalEnvelope(e *Envelope) ([]byte, error) {
	pbEnv := &pb.EventEnvelope{
		EventId:       e.EventID,
		AggregateId:   e.AggregateID,
		AggregateType: e.AggregateType,
		BenchmarkId:   e.BenchmarkID,
		EventType:     e.EventType,
		Version:       e.Version,
		Metadata:      e.Metadata,
	}
	if pbEnv.Metadata == nil {
		pbEnv.Metadata = make(map[string]string)
	}
	if len(e.Payload) > 0 {
		pbEnv.Metadata["payload_base64"] = base64.StdEncoding.EncodeToString(e.Payload)
		pbEnv.Metadata["payload_format"] = "opaque_base64"
	}
	b, err := proto.Marshal(pbEnv)
	if err != nil {
		return nil, fmt.Errorf("proto marshal: %w", err)
	}
	return b, nil
}

func unmarshalEnvelope(b []byte, out *Envelope) error {
	pbEnv := &pb.EventEnvelope{}
	if err := proto.Unmarshal(b, pbEnv); err != nil {
		return fmt.Errorf("proto unmarshal: %w", err)
	}
	out.EventID = pbEnv.EventId
	out.AggregateID = pbEnv.AggregateId
	out.AggregateType = pbEnv.AggregateType
	out.BenchmarkID = pbEnv.BenchmarkId
	out.EventType = pbEnv.EventType
	out.Version = pbEnv.Version
	out.Metadata = pbEnv.Metadata
	if out.Metadata != nil {
		if s, ok := out.Metadata["payload_base64"]; ok {
			p, err := base64.StdEncoding.DecodeString(s)
			if err == nil {
				out.Payload = p
			}
		}
	}
	return nil
}
