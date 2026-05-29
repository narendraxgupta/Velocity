// =============================================================================
//  plugin-validator-go — reference Velocity plugin.
//
//  Implements a strict self-trade-prevention validator: any fill where
//  the bid order and offer order share an account-tag is flagged as a
//  Violation with severity ERROR.
//
//  Production plugins should use a real metrics library (the
//  orchestrator scrapes `:9090/metrics` Prometheus-style if exposed)
//  and structured logging. We keep this stub minimal so the contract
//  is obvious.
// =============================================================================

package main

import (
	"context"
	"flag"
	"log"
	"net"
	"strings"
	"sync/atomic"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/protobuf/types/known/emptypb"

	// NB: import path will be:
	// pluginv1 "github.com/velocity/platform/proto/gen/go/plugin/v1"
	// ...once `buf generate` has produced the stubs. The plugin author
	// updates this import after running codegen.
	// pluginv1 "github.com/velocity/platform/proto/gen/go/plugin/v1"
)

// To keep the example self-contained we hand-roll the few types we
// need from the plugin proto. In a real plugin you'd `import pluginv1`
// instead.

type Violation struct {
	EventID     string
	Severity    int32
	Code        string
	Message     string
	DetailJSON  string
	RaisedAtNs  int64
}

// ----------------------------------------------------------------------------
//  Hand-rolled server impl below for the example. Production plugins
//  embed pluginv1.UnimplementedPluginValidatorServer and let the gRPC
//  codegen handle the wire details.
// ----------------------------------------------------------------------------

type server struct {
	validateCount  atomic.Int64
	violationCount atomic.Int64
}

func (s *server) GetInfo(_ context.Context, _ *emptypb.Empty) (any, error) {
	// In a real plugin this returns *pluginv1.PluginInfo. Sketched
	// here as a JSON-y map for documentation purposes.
	return map[string]any{
		"protocol_version": "1.0.0",
		"display_name":     "STP Strict (Go)",
		"instance_id":      "example/stp-strict/0.1.0",
		"max_batch_size":   1024,
		"subscribed_event_kinds": []string{"EVENT_KIND_FILL"},
	}, nil
}

// validateFills is the meat of the plugin. We check the bid/offer
// account tags on every fill.
func (s *server) validateFills(submissionID, benchmarkID string,
	fills []fillStub) []Violation {

	out := make([]Violation, 0, 8)
	now := time.Now().UnixNano()
	for _, f := range fills {
		s.validateCount.Add(1)
		if f.BidAccountTag != "" && f.BidAccountTag == f.OfferAccountTag {
			out = append(out, Violation{
				EventID:    f.ID,
				Severity:   3, // SEVERITY_ERROR
				Code:       "stp.self_trade",
				Message:    "Bid and offer share account_tag — STP violated.",
				DetailJSON: `{"account_tag":"` + strings.ReplaceAll(f.BidAccountTag, `"`, `\"`) + `"}`,
				RaisedAtNs: now,
			})
			s.violationCount.Add(1)
		}
	}
	return out
}

// fillStub mirrors the fields we care about from
// velocity.telemetry.v1.FillEvent. Real plugins use the generated
// pluginv1.FillEvent type — much shorter.
type fillStub struct {
	ID              string
	BidAccountTag   string
	OfferAccountTag string
}

func main() {
	grpcAddr := flag.String("grpc", ":50061", "gRPC listen address")
	flag.Parse()

	lis, err := net.Listen("tcp", *grpcAddr)
	if err != nil {
		log.Fatalf("listen: %v", err)
	}
	gs := grpc.NewServer()
	srv := &server{}
	_ = srv
	// Real plugins call:
	//   pluginv1.RegisterPluginValidatorServer(gs, srv)
	// Skipped here because pluginv1 is generated and not yet imported.

	log.Printf("STP-strict plugin listening on %s", *grpcAddr)
	if err := gs.Serve(lis); err != nil {
		log.Fatalf("serve: %v", err)
	}
}
