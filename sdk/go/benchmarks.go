package velocity

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"
)

type Profile string

const (
	ProfileBaseline    Profile = "baseline"
	ProfileSpike       Profile = "spike"
	ProfileFireHose    Profile = "fire-hose"
	ProfileAdversarial Profile = "adversarial"
	ProfileCliffFinder Profile = "cliff-finder"
	ProfileCrossVenue  Profile = "cross-venue"
)

type StartBenchmarkRequest struct {
	SubmissionID string  `json:"submission_id"`
	Profile      Profile `json:"profile"`
}

type Benchmark struct {
	ID           string  `json:"id"`
	SubmissionID string  `json:"submission_id"`
	Profile      Profile `json:"profile"`
	StartedAtNs  int64   `json:"started_at_ns"`
}

type LatencyBucket struct {
	P50  int64 `json:"p50"`
	P90  int64 `json:"p90"`
	P99  int64 `json:"p99"`
	P999 int64 `json:"p999"`
	Max  int64 `json:"max"`
}

type WatchEvent struct {
	BenchmarkID string        `json:"benchmark_id"`
	Phase       string        `json:"phase"` // "warmup"|"hold"|"cooldown"|"complete"|"cancelled"
	RPS         float64       `json:"rps"`
	LatencyNs   LatencyBucket `json:"latency_ns"`
	Score       float64       `json:"score"`
	Ts          int64         `json:"ts_ns"`
}

// BenchmarksResource bundles all /v1/benchmarks endpoints.
type BenchmarksResource struct{ c *Client }

func (r *BenchmarksResource) Start(ctx context.Context, req *StartBenchmarkRequest) (*Benchmark, error) {
	body, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}
	var resp struct {
		Benchmark Benchmark `json:"benchmark"`
	}
	if err := r.c.do(ctx, "POST", "/v1/benchmarks",
		bytes.NewReader(body), &resp); err != nil {
		return nil, err
	}
	return &resp.Benchmark, nil
}

func (r *BenchmarksResource) Cancel(ctx context.Context, id string) error {
	return r.c.do(ctx, "POST", "/v1/benchmarks/"+id+"/cancel", nil, nil)
}

// Watch opens an SSE stream and pushes parsed events to the returned
// channel. The channel is closed when the stream ends (benchmark
// complete or cancelled) or when ctx is cancelled.
//
// Errors during the stream are NOT returned via the channel — they're
// logged to a dedicated error channel via WatchWithErrors. Most
// callers don't care; if you do, use WatchWithErrors.
func (r *BenchmarksResource) Watch(ctx context.Context, benchmarkID string) (<-chan WatchEvent, error) {
	events, _, err := r.WatchWithErrors(ctx, benchmarkID)
	return events, err
}

// WatchWithErrors is the form that surfaces stream-level errors.
func (r *BenchmarksResource) WatchWithErrors(ctx context.Context, benchmarkID string) (<-chan WatchEvent, <-chan error, error) {
	req, err := http.NewRequestWithContext(ctx, "GET",
		r.c.cfg.baseURL+"/v1/benchmarks/"+benchmarkID+"/watch", nil)
	if err != nil {
		return nil, nil, err
	}
	req.Header.Set("Accept", "text/event-stream")
	req.Header.Set("User-Agent", r.c.cfg.userAgent)
	if r.c.bearer != "" {
		req.Header.Set("Authorization", "Bearer "+r.c.bearer)
	}

	// Use a long-poll client (no Timeout) since SSE is a streaming
	// response. The parent context still drives cancellation.
	streamer := &http.Client{}
	resp, err := streamer.Do(req)
	if err != nil {
		return nil, nil, err
	}
	if resp.StatusCode >= 400 {
		err := decodeError(resp)
		_ = resp.Body.Close()
		return nil, nil, err
	}

	events := make(chan WatchEvent, 16)
	errs   := make(chan error, 1)
	go func() {
		defer resp.Body.Close()
		defer close(events)
		defer close(errs)

		reader := bufio.NewReader(resp.Body)
		var dataBuf strings.Builder
		for {
			line, err := reader.ReadString('\n')
			if err != nil {
				if !errors.Is(err, io.EOF) && !errors.Is(err, context.Canceled) {
					errs <- err
				}
				return
			}
			line = strings.TrimRight(line, "\r\n")
			if line == "" {
				if dataBuf.Len() > 0 {
					var ev WatchEvent
					if err := json.Unmarshal([]byte(dataBuf.String()), &ev); err == nil {
						select {
						case events <- ev:
						case <-ctx.Done():
							return
						}
					}
					dataBuf.Reset()
				}
				continue
			}
			if strings.HasPrefix(line, "data:") {
				if dataBuf.Len() > 0 {
					dataBuf.WriteByte('\n')
				}
				dataBuf.WriteString(strings.TrimSpace(strings.TrimPrefix(line, "data:")))
			}
		}
	}()
	return events, errs, nil
}
