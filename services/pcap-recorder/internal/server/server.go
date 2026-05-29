// Package server hosts the HTTP control-plane for pcap-recorder.
//
// Identical shape to chaos-orchestrator: small JSON API, no protobuf at the
// edge. The api-gateway proxies these routes through `/v1/pcaps/...` and
// `/v1/recorder/...` so the frontend never has to know about the recorder
// service's address directly.
package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"time"

	"go.uber.org/zap"

	"github.com/velocity/platform/services/pcap-recorder/internal/recorder"
)

// Config holds the HTTP server's tunables.
type Config struct {
	Addr       string
	Logger     *zap.SugaredLogger
	Recorder   *recorder.Recorder
	Namespaces string
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

	mux.HandleFunc("POST /v1/recorder/start", s.wrap(s.start))
	mux.HandleFunc("POST /v1/recorder/stop", s.wrap(s.stop))
	mux.HandleFunc("GET /v1/recorder/state", s.wrap(s.state))
	mux.HandleFunc("GET /v1/recorder/pcap/{id}", s.wrap(s.pcap))
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

type startBody struct {
	BenchmarkID string `json:"benchmark_id"`
	Namespace   string `json:"namespace"`
	Pod         string `json:"pod"`
	TargetPort  int    `json:"target_port"`
	TTLSeconds  uint32 `json:"ttl_seconds"`
}

func (s *Server) start(w http.ResponseWriter, r *http.Request) error {
	var req startBody
	if err := decode(r, &req); err != nil {
		return err
	}
	err := s.cfg.Recorder.Start(r.Context(), recorder.StartArgs{
		BenchmarkID: req.BenchmarkID,
		Namespace:   req.Namespace,
		Pod:         req.Pod,
		TargetPort:  req.TargetPort,
		TTL:         time.Duration(req.TTLSeconds) * time.Second,
	})
	if err != nil {
		return err
	}
	return writeJSON(w, http.StatusAccepted, map[string]string{"status": "recording"})
}

type stopBody struct {
	BenchmarkID string `json:"benchmark_id"`
}

func (s *Server) stop(w http.ResponseWriter, r *http.Request) error {
	var req stopBody
	if err := decode(r, &req); err != nil {
		return err
	}
	key, size, err := s.cfg.Recorder.Stop(r.Context(), req.BenchmarkID)
	if err != nil {
		return err
	}
	return writeJSON(w, http.StatusOK, map[string]any{
		"object_key": key,
		"size_bytes": size,
	})
}

func (s *Server) state(w http.ResponseWriter, _ *http.Request) error {
	return writeJSON(w, http.StatusOK, map[string]any{
		"allowed_namespaces": s.cfg.Namespaces,
		"active":             s.cfg.Recorder.Active(),
		"server_ts_ns":       time.Now().UnixNano(),
	})
}

// pcap surfaces the presigned download URL for a finalised capture.
// Returns 404 if no object exists under `pcaps/<id>.pcap` — i.e. the
// recorder never finalised this benchmark (maybe it was chaos-killed
// before Stop), or the benchmark_id is just wrong.
func (s *Server) pcap(w http.ResponseWriter, r *http.Request) error {
	id := r.PathValue("id")
	if id == "" {
		return badRequest("missing id")
	}
	key := fmt.Sprintf("pcaps/%s.pcap", id)
	size, _, err := s.cfg.Recorder.Storage().Stat(r.Context(), key)
	if err != nil {
		return &userError{code: http.StatusNotFound,
			msg: fmt.Sprintf("no pcap for %s: %v", id, err)}
	}
	url, err := s.cfg.Recorder.Storage().PresignGet(r.Context(), key, 15*time.Minute)
	if err != nil {
		return err
	}
	return writeJSON(w, http.StatusOK, map[string]any{
		"benchmark_id": id,
		"object_key":   key,
		"size_bytes":   size,
		"download_url": url,
		"expires_in":   "15m",
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

// wrap turns an error-returning handler into net/http.HandlerFunc with
// consistent error mapping and structured logging.
func (s *Server) wrap(h func(http.ResponseWriter, *http.Request) error) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if err := h(w, r); err != nil {
			s.cfg.Logger.Warnw("pcap handler error",
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
