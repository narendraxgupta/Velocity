// Package prompt builds the prompts we feed the local LLM for strategy
// critique. The actual prompts live here because (a) they're stable and
// version-controlled rather than magic strings, and (b) you can unit
// test the assembly without standing up the LLM.
//
// We never give the LLM a free hand — the prompt is structured to
// produce a JSON document with named fields so the gateway and the
// frontend can render it without surprises. The model is asked to
// reason about microstructure, latency, and correctness based on the
// concrete benchmark report attached, not just to riff on the source.
package prompt

import (
	"encoding/json"
	"fmt"
	"strings"
)

// SubmissionContext is the structured input we hand to the LLM. The
// gateway populates it from the submission record, benchmark report,
// and execution-quality snapshot.
type SubmissionContext struct {
	SubmissionID    string  `json:"submission_id"`
	Language        string  `json:"language"`
	Kind            string  `json:"kind"`
	StrategyName    string  `json:"strategy_name,omitempty"`
	StrategyVersion string  `json:"strategy_version,omitempty"`

	// Source code excerpt. We truncate to ~80KB before sending — local
	// models OOM on giant contexts and longer files always have
	// pre-existing summaries / headers that distil the strategy.
	Source string `json:"source"`

	// Benchmark observations (point-in-time at submission status read).
	Score                 float64 `json:"score"`
	ThroughputRps         float64 `json:"throughput_rps"`
	P50LatencyNs          float64 `json:"p50_latency_ns"`
	P99LatencyNs          float64 `json:"p99_latency_ns"`
	P999LatencyNs         float64 `json:"p999_latency_ns"`
	CorrectnessRatio      float64 `json:"correctness_ratio"`
	OrderRejectRate       float64 `json:"order_reject_rate"`
	SlippageBps           float64 `json:"slippage_bps,omitempty"`
	ImplementationShortfallBps float64 `json:"is_bps,omitempty"`
	ReversionBps          float64 `json:"reversion_bps,omitempty"`
	KernelUserspaceSkewNs float64 `json:"kernel_userspace_skew_ns,omitempty"`
}

// SystemPrompt is the LLM's persona. We're explicit that this is for
// quant developers reviewing a TRADING strategy, not a generic code
// review.
const SystemPrompt = `You are a senior quantitative trading engineer reviewing a low-latency strategy submitted to a competitive benchmarking platform.
Your role is to find concrete, actionable improvements grounded in the benchmark observations you are given.

You speak fluently about market microstructure (limit-order books, FIFO vs pro-rata, queue position, adverse selection),
low-latency engineering (kernel bypass, lock-free data structures, NUMA, branch prediction, cache locality), and
correctness invariants for trading systems (partial fills, idempotency, time-in-force, self-trade prevention).

You NEVER fabricate numbers. If the source or report doesn't support a claim, you say so.
You do NOT moralise about strategy ethics or "responsible trading" — assume the strategy is in a legitimate competition.`

// UserPromptTemplate produces the user-side prompt for the model. The
// model is instructed to emit a strict JSON shape; we'll validate the
// response before storing it.
//
// The structure includes:
//   summary           — 1-paragraph executive summary
//   strengths[]       — concrete things the strategy does well
//   risks[]           — observed or likely failure modes
//   suggestions[]     — actionable, ordered by impact
//   microstructure[]  — microstructure-specific observations
//   latency[]         — latency-engineering observations
//   correctness[]     — invariants likely to break under load
func UserPromptTemplate(ctx SubmissionContext) (string, error) {
	// We re-marshal SubmissionContext so the model sees a self-describing
	// JSON block — same field names as our schema. Source is excluded
	// from the JSON envelope and rendered as a fenced code block to make
	// the boundary explicit (and to keep the JSON small).
	jsonOnly := ctx
	jsonOnly.Source = ""
	envelope, err := json.MarshalIndent(jsonOnly, "", "  ")
	if err != nil {
		return "", err
	}

	var b strings.Builder
	b.WriteString("BENCHMARK_REPORT:\n")
	b.WriteString("```json\n")
	b.Write(envelope)
	b.WriteString("\n```\n\n")

	b.WriteString("SUBMISSION_SOURCE (")
	b.WriteString(ctx.Language)
	b.WriteString(", truncated to first 80kB):\n```\n")
	src := ctx.Source
	const maxBytes = 80_000
	if len(src) > maxBytes {
		src = src[:maxBytes] + "\n/* ... truncated ... */\n"
	}
	b.WriteString(src)
	b.WriteString("\n```\n\n")

	b.WriteString(`Produce a critique as a single JSON object with EXACTLY these fields:

{
  "summary":         string,
  "strengths":       string[],
  "risks":           string[],
  "suggestions":     [ { "priority": "high"|"medium"|"low", "title": string, "rationale": string } ],
  "microstructure":  string[],
  "latency":         string[],
  "correctness":     string[]
}

Rules:
- Output ONLY the JSON object. No prose before or after.
- Cite specific numbers from the benchmark report when relevant (e.g. "p99 of 312µs at 18k RPS").
- Be terse and concrete. Bullet items max 30 words. Suggestions sorted by expected impact descending.
- 3 to 5 items per list, no more. Empty arrays are allowed when nothing relevant applies.
`)

	return b.String(), nil
}

// Critique is the validated, post-parsed response shape we persist.
// Fields are tagged to match the JSON the model is told to produce.
type Critique struct {
	Summary        string       `json:"summary"`
	Strengths      []string     `json:"strengths"`
	Risks          []string     `json:"risks"`
	Suggestions    []Suggestion `json:"suggestions"`
	Microstructure []string     `json:"microstructure"`
	Latency        []string     `json:"latency"`
	Correctness    []string     `json:"correctness"`
}

// Suggestion is a single actionable proposal.
type Suggestion struct {
	Priority  string `json:"priority"` // "high" | "medium" | "low"
	Title     string `json:"title"`
	Rationale string `json:"rationale"`
}

// ParseAndValidate strips common LLM artifacts (leading "```json"
// fences, trailing prose) and validates the document conforms to the
// Critique schema. Returns the parsed value plus the cleaned raw text
// (useful for debug logs).
func ParseAndValidate(raw string) (*Critique, string, error) {
	cleaned := stripFences(raw)
	var out Critique
	if err := json.Unmarshal([]byte(cleaned), &out); err != nil {
		return nil, cleaned, fmt.Errorf("critique JSON decode: %w", err)
	}
	if out.Summary == "" {
		return nil, cleaned, fmt.Errorf("critique missing 'summary'")
	}
	for i, s := range out.Suggestions {
		switch s.Priority {
		case "high", "medium", "low":
		default:
			return nil, cleaned,
				fmt.Errorf("suggestion[%d] has invalid priority %q", i, s.Priority)
		}
	}
	return &out, cleaned, nil
}

func stripFences(s string) string {
	s = strings.TrimSpace(s)
	// Strip surrounding ```json ... ``` if present.
	if strings.HasPrefix(s, "```") {
		if idx := strings.Index(s, "\n"); idx >= 0 {
			s = s[idx+1:]
		}
		if i := strings.LastIndex(s, "```"); i >= 0 {
			s = s[:i]
		}
	}
	// Sometimes the model adds a prose preface before the JSON. Cut to
	// the first `{` we see at the start of a line.
	if i := strings.Index(s, "{"); i > 0 {
		s = s[i:]
	}
	// And sometimes a trailing comment after the closing brace.
	if i := strings.LastIndex(s, "}"); i >= 0 {
		s = s[:i+1]
	}
	return strings.TrimSpace(s)
}
