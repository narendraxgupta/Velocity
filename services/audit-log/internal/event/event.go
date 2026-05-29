// Package event defines the canonical audit event schema.
//
// The schema is intentionally narrow — we want consistency across all
// emitters more than we want flexibility. New fields go in `Meta` (a
// JSON object) until we see them often enough to promote them to
// first-class columns.
package event

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"time"
)

// Action is a stable, lower-snake_case verb naming what happened.
// Examples: "submission.create", "benchmark.start", "chaos.inject".
type Action string

// Outcome is success / failure of the action. Strings on the wire keep
// the audit table human-greppable.
type Outcome string

const (
	OutcomeAllow Outcome = "allow"
	OutcomeDeny  Outcome = "deny"
	OutcomeError Outcome = "error"
)

// Event is the JSON shape every emitter produces and every consumer
// reads. Adding a field is a breaking change for chain-hash readers —
// bump SchemaVersion at the same time.
type Event struct {
	SchemaVersion int            `json:"v"`
	EventID       string         `json:"event_id"`     // ULID
	TenantID      string         `json:"tenant_id"`
	Subject       string         `json:"subject"`      // JWT sub
	Role          string         `json:"role"`
	SourceService string         `json:"source"`       // "api-gateway"
	Action        Action         `json:"action"`
	ResourceType  string         `json:"resource_type"`
	ResourceID    string         `json:"resource_id"`
	Outcome       Outcome        `json:"outcome"`
	StatusCode    int            `json:"status_code"`
	OccurredAtNs  int64          `json:"occurred_at_ns"`
	RemoteIP      string         `json:"remote_ip"`
	RequestID     string         `json:"request_id"`
	Meta          map[string]any `json:"meta,omitempty"`

	// ChainHash is computed by the consumer, NOT the emitter. Emitters
	// MUST leave it empty.
	ChainHash string `json:"chain_hash,omitempty"`
}

// Validate enforces non-empty critical fields. Returns a single
// concatenated error message so callers can log it directly.
func (e *Event) Validate() error {
	var bad []string
	if e.EventID == "" {
		bad = append(bad, "event_id")
	}
	if e.TenantID == "" {
		bad = append(bad, "tenant_id")
	}
	if e.Action == "" {
		bad = append(bad, "action")
	}
	if e.SourceService == "" {
		bad = append(bad, "source")
	}
	if e.OccurredAtNs == 0 {
		bad = append(bad, "occurred_at_ns")
	}
	if e.SchemaVersion == 0 {
		bad = append(bad, "v")
	}
	if len(bad) > 0 {
		return errors.New("event missing required fields: " + strings.Join(bad, ","))
	}
	return nil
}

// ComputeChainHash sets ChainHash to SHA-256(prevHash || canonical-JSON).
// Canonical-JSON here is sort-key marshalled (encoding/json does that
// by default for maps) with the chain_hash field removed.
//
// We use SHA-256 over an HMAC because the chain is intended for
// *integrity*, not authenticity — anyone with write access to QuestDB
// can re-hash a forged row. The point is to make tampering *detectable*
// via off-cluster verification (cmd/audit-verify reads the chain and
// fails noisily on any mismatch).
func (e *Event) ComputeChainHash(prevHash string) error {
	e.ChainHash = ""
	body, err := json.Marshal(e)
	if err != nil {
		return err
	}
	h := sha256.New()
	h.Write([]byte(prevHash))
	h.Write(body)
	e.ChainHash = hex.EncodeToString(h.Sum(nil))
	return nil
}

// HelperNowNs returns the current wall-clock time in nanoseconds.
// Callers should prefer this over inlining time.Now().UnixNano() so we
// have a single seam to mock in tests.
func HelperNowNs() int64 { return time.Now().UnixNano() }
