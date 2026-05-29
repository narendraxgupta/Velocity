// Package velocity is the public Go client for the Velocity API.
//
// Two transports are supported in the same client:
//
//   - HTTP/1.1 against the api-gateway for resource CRUD and SSE
//     streams. This is the primary path; the gateway already imposes
//     RBAC, audit, and rate limiting.
//   - gRPC against the submission-engine + bot-controller. Currently
//     unused by the public surface but exposed for advanced callers
//     who don't want to round-trip through HTTP. mTLS only.
//
// The SDK is deliberately thin: it adds typing, retries, and a Watch()
// helper for SSE streams; it does NOT cache or transform server
// responses beyond JSON decode. Callers that want richer behaviour
// build on top.
package velocity

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

// Client is the entry point. It's safe to share across goroutines.
type Client struct {
	cfg      clientConfig
	http     *http.Client
	bearer   string

	Submissions *SubmissionsResource
	Benchmarks  *BenchmarksResource
	Leaderboard *LeaderboardResource
	Audit       *AuditResource
}

// Option configures a Client. The functional-options pattern keeps the
// constructor backwards-compatible as we add knobs.
type Option func(*clientConfig)

type clientConfig struct {
	baseURL     string
	bearer      string
	timeout     time.Duration
	userAgent   string
	maxRetries  int
}

func defaultConfig() clientConfig {
	return clientConfig{
		baseURL:    "http://localhost:8080",
		timeout:    30 * time.Second,
		userAgent:  "velocity-sdk-go/0.1",
		maxRetries: 2,
	}
}

func WithBaseURL(url string) Option    { return func(c *clientConfig) { c.baseURL = strings.TrimRight(url, "/") } }
func WithBearer(token string) Option   { return func(c *clientConfig) { c.bearer = token } }
func WithTimeout(d time.Duration) Option { return func(c *clientConfig) { c.timeout = d } }
func WithUserAgent(ua string) Option   { return func(c *clientConfig) { c.userAgent = ua } }
func WithMaxRetries(n int) Option      { return func(c *clientConfig) { c.maxRetries = n } }

// NewClient constructs a Client. Returns an error if no base URL can
// be resolved.
func NewClient(opts ...Option) (*Client, error) {
	cfg := defaultConfig()
	for _, o := range opts {
		o(&cfg)
	}
	if cfg.baseURL == "" {
		return nil, errors.New("velocity: base URL is required")
	}
	c := &Client{
		cfg:    cfg,
		bearer: cfg.bearer,
		http:   &http.Client{Timeout: cfg.timeout},
	}
	c.Submissions = &SubmissionsResource{c: c}
	c.Benchmarks  = &BenchmarksResource{c: c}
	c.Leaderboard = &LeaderboardResource{c: c}
	c.Audit       = &AuditResource{c: c}
	return c, nil
}

// ----------------------------------------------------------------------------
//  Internal: request plumbing
// ----------------------------------------------------------------------------

func (c *Client) do(ctx context.Context, method, path string,
	body io.Reader, out any) error {
	// Buffer the request body up-front so retries can re-issue it.
	// Without this, a transient 5xx triggers a retry against an already-
	// consumed Reader and the gateway sees an empty POST. We only ever
	// send small JSON bodies through this helper; large payloads (file
	// uploads) deliberately bypass it via the resource-specific upload()
	// methods.
	var bodyBytes []byte
	if body != nil {
		var err error
		bodyBytes, err = io.ReadAll(body)
		if err != nil {
			return err
		}
	}

	newReq := func() (*http.Request, error) {
		var rdr io.Reader
		if bodyBytes != nil {
			rdr = bytes.NewReader(bodyBytes)
		}
		req, err := http.NewRequestWithContext(ctx, method, c.cfg.baseURL+path, rdr)
		if err != nil {
			return nil, err
		}
		req.Header.Set("User-Agent", c.cfg.userAgent)
		req.Header.Set("Accept", "application/json")
		if bodyBytes != nil {
			req.Header.Set("Content-Type", "application/json")
		}
		if c.bearer != "" {
			req.Header.Set("Authorization", "Bearer "+c.bearer)
		}
		return req, nil
	}

	var (
		resp *http.Response
		err  error
	)
	for attempt := 0; attempt <= c.cfg.maxRetries; attempt++ {
		req, reqErr := newReq()
		if reqErr != nil {
			return reqErr
		}
		// On retries we discard the previous attempt's body so we don't
		// leak the connection. The final attempt's body (success OR 5xx)
		// is left open for the post-loop response handler below.
		if resp != nil {
			_ = resp.Body.Close()
			resp = nil
		}
		resp, err = c.http.Do(req)
		if err == nil && resp.StatusCode < 500 {
			break
		}
		if attempt == c.cfg.maxRetries {
			// Out of retries. If the transport returned an error,
			// surface that. Otherwise we fall through and let the
			// downstream handler turn the 5xx response into an APIError.
			if err != nil {
				return err
			}
			break
		}
		// Exponential backoff with jitter — capped at 1s so tests stay
		// fast and an operator can ctrl-C without waiting forever.
		sleep := time.Duration(1<<attempt)*100*time.Millisecond + time.Duration(int64(time.Millisecond)*int64(attempt*37%100))
		if sleep > time.Second {
			sleep = time.Second
		}
		select {
		case <-time.After(sleep):
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	if resp == nil {
		// Defensive: the only way to reach here is err != nil on every
		// attempt, which the loop above should already have returned.
		if err != nil {
			return err
		}
		return errors.New("velocity: no response after retries")
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 400 {
		return decodeError(resp)
	}
	if out == nil {
		return nil
	}
	return json.NewDecoder(resp.Body).Decode(out)
}

func decodeError(resp *http.Response) error {
	body, _ := io.ReadAll(resp.Body)
	var apierr struct {
		Error string `json:"error"`
	}
	_ = json.Unmarshal(body, &apierr)
	if apierr.Error == "" {
		apierr.Error = strings.TrimSpace(string(body))
	}
	return &APIError{
		StatusCode: resp.StatusCode,
		Message:    apierr.Error,
	}
}

// APIError is returned for any non-2xx response.
type APIError struct {
	StatusCode int
	Message    string
}

func (e *APIError) Error() string {
	return fmt.Sprintf("velocity: HTTP %d — %s", e.StatusCode, e.Message)
}
