// Package ollama wraps the local Ollama generate API.
//
// We talk to a self-hosted Ollama instance for a few important reasons:
//
//   - Submitter code is sensitive. Sending it to a hosted LLM provider is
//     a compliance non-starter for hedge funds, prop shops, and exchanges
//     — the exact buyers we want to court.
//   - Cost. Critique latency is a few seconds per submission; over a
//     league of 200 submissions a week that's 800 large-context calls a
//     month. Hosted pricing kills the budget; a single GPU node on
//     llama3:8b-instruct or qwen2.5-coder:7b handles the volume.
//   - Determinism. Ollama exposes the `seed` parameter; we feed it the
//     submission ULID so re-running the critique produces the same
//     output (or close to it). Useful for audit trails.
//
// The wire format is small and stable enough that we don't pull in the
// official Go client (which has an outsized dep graph for what we use).
package ollama

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// Client is a thin Ollama /api/generate wrapper.
type Client struct {
	BaseURL string         // e.g. http://ollama:11434
	Model   string         // e.g. qwen2.5-coder:7b
	HTTP    *http.Client   // configurable for unit tests
	Timeout time.Duration  // request timeout, applied per call
}

// New builds a Client with sane defaults.
func New(baseURL, model string) *Client {
	if baseURL == "" {
		baseURL = "http://ollama:11434"
	}
	if model == "" {
		model = "qwen2.5-coder:7b"
	}
	return &Client{
		BaseURL: strings.TrimRight(baseURL, "/"),
		Model:   model,
		HTTP:    &http.Client{Timeout: 120 * time.Second},
		Timeout: 120 * time.Second,
	}
}

// GenerateRequest mirrors Ollama's /api/generate request body. We only
// expose the fields we actually set; defaults on the server are fine.
type GenerateRequest struct {
	Model   string                 `json:"model"`
	Prompt  string                 `json:"prompt"`
	System  string                 `json:"system,omitempty"`
	Stream  bool                   `json:"stream"`
	Options map[string]any         `json:"options,omitempty"`
	Format  string                 `json:"format,omitempty"` // "" or "json"
	KeepAlive string               `json:"keep_alive,omitempty"`
}

// GenerateResponse is Ollama's non-streaming response.
type GenerateResponse struct {
	Model     string    `json:"model"`
	Response  string    `json:"response"`
	Done      bool      `json:"done"`
	CreatedAt time.Time `json:"created_at"`
	// Token accounting — handy for cost dashboards.
	PromptEvalCount int `json:"prompt_eval_count,omitempty"`
	EvalCount       int `json:"eval_count,omitempty"`
	TotalDuration   int64 `json:"total_duration,omitempty"` // nanoseconds
}

// Generate calls /api/generate with stream=false and parses the single
// response object. We force stream=false because the critique pipeline
// is a request/response, not a chat — we don't need token-by-token
// streaming and the simpler single-shot path is easier to reason about.
func (c *Client) Generate(ctx context.Context, req GenerateRequest) (*GenerateResponse, error) {
	if c.BaseURL == "" {
		return nil, errors.New("ollama: empty base URL")
	}
	req.Stream = false
	if req.Model == "" {
		req.Model = c.Model
	}
	body, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("ollama: marshal request: %w", err)
	}

	if c.Timeout > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, c.Timeout)
		defer cancel()
	}
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost,
		c.BaseURL+"/api/generate", bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("ollama: build request: %w", err)
	}
	httpReq.Header.Set("Content-Type", "application/json")
	resp, err := c.HTTP.Do(httpReq)
	if err != nil {
		return nil, fmt.Errorf("ollama: http: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		raw, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return nil, fmt.Errorf("ollama: status %d: %s", resp.StatusCode, raw)
	}
	var out GenerateResponse
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, fmt.Errorf("ollama: decode: %w", err)
	}
	return &out, nil
}

// Healthz pings /api/tags as a cheap liveness check.
func (c *Client) Healthz(ctx context.Context) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet,
		c.BaseURL+"/api/tags", nil)
	if err != nil {
		return err
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("ollama healthz: status %d", resp.StatusCode)
	}
	return nil
}
