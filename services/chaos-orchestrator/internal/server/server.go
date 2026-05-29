// Package server hosts the HTTP control-plane for chaos-orchestrator.
//
// HTTP instead of gRPC because: (a) chaos actions are operator-driven, not
// inner-loop; the marginal protobuf overhead doesn't help us, (b) the
// frontend's admin panel can talk to it directly through the api-gateway's
// passthrough, and (c) testing with `curl` is trivial.
package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"time"

	"go.uber.org/zap"

	"github.com/velocity/platform/services/chaos-orchestrator/internal/injector"
)

// Config carries the HTTP server's tunables.
type Config struct {
	Addr       string
	Logger     *zap.SugaredLogger
	Injector   *injector.Injector
	Namespaces string  // pass-through for /v1/chaos/status
}

// Server is the long-lived process object.
type Server struct {
	cfg  Config
	http *http.Server
}

// New constructs the Server but doesn't yet start it.
func New(cfg Config) *Server {
	mux := http.NewServeMux()
	s := &Server{cfg: cfg}

	mux.HandleFunc("GET  /v1/chaos/status",         s.wrap(s.status))
	mux.HandleFunc("POST /v1/chaos/pod-kill",       s.wrap(s.podKill))
	mux.HandleFunc("POST /v1/chaos/tc-latency",     s.wrap(s.tcLatency))
	mux.HandleFunc("POST /v1/chaos/tc-loss",        s.wrap(s.tcLoss))
	mux.HandleFunc("POST /v1/chaos/cpu-throttle",   s.wrap(s.cpuThrottle))
	mux.HandleFunc("POST /v1/chaos/partition",      s.wrap(s.partition))
	mux.HandleFunc("GET  /healthz",                 s.wrap(s.healthz))

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

type podSelector struct {
	Namespace string `json:"namespace"`
	Pod       string `json:"pod"`
}

func (s *Server) status(w http.ResponseWriter, r *http.Request) error {
	return writeJSON(w, http.StatusOK, map[string]any{
		"allowed_namespaces": s.cfg.Namespaces,
		"active":             s.cfg.Injector.Active(),
		"server_ts_ns":       time.Now().UnixNano(),
	})
}

func (s *Server) podKill(w http.ResponseWriter, r *http.Request) error {
	var req podSelector
	if err := decode(r, &req); err != nil {
		return err
	}
	if err := s.cfg.Injector.PodKill(r.Context(), req.Namespace, req.Pod); err != nil {
		return err
	}
	return writeJSON(w, http.StatusAccepted, map[string]string{"status": "killed"})
}

type latencyBody struct {
	podSelector
	DelayMs    uint32 `json:"delay_ms"`
	JitterMs   uint32 `json:"jitter_ms"`
	DurationS  uint32 `json:"duration_s"`
}

func (s *Server) tcLatency(w http.ResponseWriter, r *http.Request) error {
	var req latencyBody
	if err := decode(r, &req); err != nil {
		return err
	}
	if err := s.cfg.Injector.TCLatency(r.Context(), req.Namespace, req.Pod,
		injector.LatencyArgs{
			DelayMs:  req.DelayMs,
			JitterMs: req.JitterMs,
			Duration: time.Duration(req.DurationS) * time.Second,
		}); err != nil {
		return err
	}
	return writeJSON(w, http.StatusAccepted, map[string]string{"status": "injected"})
}

type lossBody struct {
	podSelector
	LossPct    uint32 `json:"loss_pct"`
	DurationS  uint32 `json:"duration_s"`
}

func (s *Server) tcLoss(w http.ResponseWriter, r *http.Request) error {
	var req lossBody
	if err := decode(r, &req); err != nil {
		return err
	}
	if err := s.cfg.Injector.TCLoss(r.Context(), req.Namespace, req.Pod,
		injector.LossArgs{
			LossPct:  req.LossPct,
			Duration: time.Duration(req.DurationS) * time.Second,
		}); err != nil {
		return err
	}
	return writeJSON(w, http.StatusAccepted, map[string]string{"status": "injected"})
}

type cpuBody struct {
	podSelector
	CPUMaxMicros uint32 `json:"cpu_max_micros"`
	DurationS    uint32 `json:"duration_s"`
}

func (s *Server) cpuThrottle(w http.ResponseWriter, r *http.Request) error {
	var req cpuBody
	if err := decode(r, &req); err != nil {
		return err
	}
	if err := s.cfg.Injector.CPUThrottle(r.Context(), req.Namespace, req.Pod,
		injector.CPUThrottleArgs{
			CPUMaxMicros: req.CPUMaxMicros,
			Duration:     time.Duration(req.DurationS) * time.Second,
		}); err != nil {
		return err
	}
	return writeJSON(w, http.StatusAccepted, map[string]string{"status": "injected"})
}

type partBody struct {
	podSelector
	Upstream  string `json:"upstream"`
	DurationS uint32 `json:"duration_s"`
}

func (s *Server) partition(w http.ResponseWriter, r *http.Request) error {
	var req partBody
	if err := decode(r, &req); err != nil {
		return err
	}
	if err := s.cfg.Injector.Partition(r.Context(), req.Namespace, req.Pod,
		injector.PartitionArgs{
			Upstream: req.Upstream,
			Duration: time.Duration(req.DurationS) * time.Second,
		}); err != nil {
		return err
	}
	return writeJSON(w, http.StatusAccepted, map[string]string{"status": "injected"})
}

func (s *Server) healthz(w http.ResponseWriter, r *http.Request) error {
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("ok"))
	return nil
}

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------

// wrap turns a handler returning `error` into a net/http HandlerFunc with
// consistent error mapping + structured logging.
func (s *Server) wrap(h func(http.ResponseWriter, *http.Request) error) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if err := h(w, r); err != nil {
			s.cfg.Logger.Warnw("chaos handler error",
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
