// Package server exposes the replayer's HTTP control-plane.
//
// Shaped to be drop-in proxyable through the api-gateway like chaos and
// recorder. The gateway adds tracing + auth at its edge; this server
// stays single-purpose.
package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"time"

	"go.uber.org/zap"

	"github.com/oklog/ulid/v2"

	"github.com/velocity/platform/services/pcap-replayer/internal/replayer"
	"github.com/velocity/platform/services/pcap-replayer/internal/store"
)

// Config holds the HTTP server's tunables.
type Config struct {
	Addr     string
	Logger   *zap.SugaredLogger
	Replayer *replayer.Replayer
	Storage  store.Storage
}

// Server is the long-lived process object.
type Server struct {
	cfg  Config
	http *http.Server
}

// New constructs the server but does not yet bind.
func New(cfg Config) *Server {
	mux := http.NewServeMux()
	s := &Server{cfg: cfg}

	mux.HandleFunc("POST /v1/pcaps/replay", s.wrap(s.startReplay))
	mux.HandleFunc("POST /v1/pcaps/{id}/cancel", s.wrap(s.cancelReplay))
	mux.HandleFunc("GET /v1/pcaps/{id}/state", s.wrap(s.watchReplay))
	mux.HandleFunc("GET /v1/pcaps", s.wrap(s.listPcaps))
	mux.HandleFunc("GET /healthz", s.wrap(s.healthz))

	s.http = &http.Server{
		Addr:              cfg.Addr,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
	}
	return s
}

// Listen blocks until the server exits.
func (s *Server) Listen() error { return s.http.ListenAndServe() }

// Shutdown drains in-flight requests.
func (s *Server) Shutdown(ctx context.Context) error { return s.http.Shutdown(ctx) }

// ----------------------------------------------------------------------------
//  Routes
// ----------------------------------------------------------------------------

type replayBody struct {
	TargetSubmissionID string  `json:"target_submission_id"`
	TargetHost         string  `json:"target_host"`
	TargetPort         int     `json:"target_port"`
	SourceObjectKey    string  `json:"source_object_key"`
	ClockMode          string  `json:"clock_mode"` // "preserve" | "fixed_rps"
	FixedRPS           uint64  `json:"fixed_rps"`
	SpeedMultiplier    float64 `json:"speed_multiplier"`
}

func (s *Server) startReplay(w http.ResponseWriter, r *http.Request) error {
	var req replayBody
	if err := decode(r, &req); err != nil {
		return err
	}

	benchID := ulid.Make().String()

	clock := replayer.ClockModePreserve
	switch req.ClockMode {
	case "fixed_rps", "FIXED_RPS":
		clock = replayer.ClockModeFixedRPS
	case "", "preserve", "PRESERVE":
		clock = replayer.ClockModePreserve
	default:
		return badRequest(fmt.Sprintf("unknown clock_mode %q", req.ClockMode))
	}

	expected, err := s.cfg.Replayer.Start(r.Context(), replayer.StartArgs{
		BenchmarkID:     benchID,
		TargetHost:      req.TargetHost,
		TargetPort:      req.TargetPort,
		SourceObjectKey: req.SourceObjectKey,
		ClockMode:       clock,
		FixedRPS:        req.FixedRPS,
		SpeedMultiplier: req.SpeedMultiplier,
	})
	if err != nil {
		return err
	}
	return writeJSON(w, http.StatusAccepted, map[string]any{
		"benchmark_id":     benchID,
		"expected_packets": expected,
		"started_at_ns":    time.Now().UnixNano(),
	})
}

func (s *Server) cancelReplay(w http.ResponseWriter, r *http.Request) error {
	id := r.PathValue("id")
	if id == "" {
		return badRequest("missing id")
	}
	if !s.cfg.Replayer.Cancel(id) {
		return notFound(fmt.Sprintf("no active replay for %s", id))
	}
	return writeJSON(w, http.StatusAccepted, map[string]string{"status": "cancelling"})
}

func (s *Server) watchReplay(w http.ResponseWriter, r *http.Request) error {
	id := r.PathValue("id")
	if id == "" {
		return badRequest("missing id")
	}
	snap := s.cfg.Replayer.Watch(id)
	if snap == nil {
		return notFound(fmt.Sprintf("no replay state for %s", id))
	}
	return writeJSON(w, http.StatusOK, snap)
}

func (s *Server) listPcaps(w http.ResponseWriter, r *http.Request) error {
	// Optional limit; cap at 200 so a hostile caller can't drag down MinIO.
	limit := 50
	if q := r.URL.Query().Get("limit"); q != "" {
		var n int
		if _, err := fmt.Sscanf(q, "%d", &n); err == nil && n > 0 && n <= 200 {
			limit = n
		}
	}
	items, err := s.cfg.Storage.ListPcaps(r.Context(), limit)
	if err != nil {
		return err
	}
	return writeJSON(w, http.StatusOK, map[string]any{
		"items": items,
		"count": len(items),
	})
}

func (s *Server) healthz(w http.ResponseWriter, _ *http.Request) error {
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("ok"))
	return nil
}

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------

func (s *Server) wrap(h func(http.ResponseWriter, *http.Request) error) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if err := h(w, r); err != nil {
			s.cfg.Logger.Warnw("replayer handler error",
				"path", r.URL.Path, "err", err)
			code := http.StatusInternalServerError
			var apiErr *userError
			if errors.As(err, &apiErr) {
				code = apiErr.code
			}
			_ = writeJSON(w, code, map[string]string{"error": err.Error()})
		}
	}
}

type userError struct {
	code int
	msg  string
}

func (e *userError) Error() string { return e.msg }

func badRequest(msg string) error { return &userError{code: http.StatusBadRequest, msg: msg} }
func notFound(msg string) error   { return &userError{code: http.StatusNotFound, msg: msg} }

func decode(r *http.Request, dst any) error {
	if r.Body == nil {
		return badRequest("missing body")
	}
	defer r.Body.Close()
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return badRequest(fmt.Sprintf("malformed JSON: %v", err))
	}
	return nil
}

func writeJSON(w http.ResponseWriter, code int, body any) error {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	return json.NewEncoder(w).Encode(body)
}
