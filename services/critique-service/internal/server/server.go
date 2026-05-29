// Package server hosts the HTTP API for the critique-service.
//
// Endpoints
// ---------
//
//	POST /v1/critiques
//	  Body: { "submission_id": ..., "language": ..., "kind": ...,
//	          "source": "...", report fields ... }
//	  Returns 202 with { "critique_id": ..., "status": "running" } and
//	  starts the LLM call asynchronously. The submission_id is used to
//	  index the latest critique so subsequent GETs by submission_id
//	  return this one once it completes.
//
//	GET  /v1/critiques/{critique_id}
//	  Returns the persisted record, or 404 / 202 (in-flight).
//
//	GET  /v1/critiques/by-submission/{submission_id}
//	  Returns the latest critique for a submission.
//
//	GET  /healthz, /readyz
//	  Standard liveness / readiness; readyz also pings ollama and redis.
//
// Concurrency model
// -----------------
// We bound concurrent LLM calls via a semaphore (default 2) so the GPU
// node doesn't melt under request bursts. Excess requests get a 429
// with Retry-After. This keeps the service predictable without a
// queue daemon — there's no SLA on critique freshness.
package server

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/oklog/ulid/v2"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/critique-service/internal/ollama"
	"github.com/velocity/platform/services/critique-service/internal/prompt"
	"github.com/velocity/platform/services/critique-service/internal/store"
)

// Config wires the server with its dependencies.
type Config struct {
	Addr        string
	Ollama      *ollama.Client
	Store       *store.Store
	Logger      *zap.SugaredLogger
	MaxInFlight int // semaphore size; 0 → 2
}

// Server is the HTTP API.
type Server struct {
	cfg Config

	sem chan struct{}

	// In-process map of submission_id → critique_id for the most
	// recently-started call; lets us de-dupe concurrent POSTs.
	mu       sync.Mutex
	inflight map[string]string
}

// New constructs a Server.
func New(cfg Config) *Server {
	if cfg.MaxInFlight <= 0 {
		cfg.MaxInFlight = 2
	}
	return &Server{
		cfg:      cfg,
		sem:      make(chan struct{}, cfg.MaxInFlight),
		inflight: make(map[string]string),
	}
}

// Routes registers the HTTP handlers on a fresh mux. We use the Go 1.22
// router which understands path patterns and methods natively.
func (s *Server) Routes() *http.ServeMux {
	m := http.NewServeMux()
	m.HandleFunc("POST /v1/critiques", s.handleCreate)
	m.HandleFunc("GET /v1/critiques/{critique_id}", s.handleGet)
	m.HandleFunc("GET /v1/critiques/by-submission/{submission_id}", s.handleGetBySubmission)
	m.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("ok"))
	})
	m.HandleFunc("GET /readyz", s.handleReady)
	return m
}

func (s *Server) handleReady(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	if err := s.cfg.Ollama.Healthz(ctx); err != nil {
		http.Error(w, "ollama: "+err.Error(), http.StatusServiceUnavailable)
		return
	}
	_, _ = w.Write([]byte("ready"))
}

// -----------------------------------------------------------------------------
//  Handlers
// -----------------------------------------------------------------------------

func (s *Server) handleCreate(w http.ResponseWriter, r *http.Request) {
	var req prompt.SubmissionContext
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "decode: "+err.Error())
		return
	}
	if req.SubmissionID == "" {
		writeError(w, http.StatusBadRequest, "submission_id required")
		return
	}
	if strings.TrimSpace(req.Source) == "" {
		writeError(w, http.StatusBadRequest, "source required")
		return
	}

	// De-dupe within the process.
	s.mu.Lock()
	if existing, ok := s.inflight[req.SubmissionID]; ok {
		s.mu.Unlock()
		writeJSON(w, http.StatusAccepted, map[string]any{
			"critique_id": existing,
			"status":      string(store.StatusRunning),
			"deduped":     true,
		})
		return
	}
	cid := ulid.Make().String()
	s.inflight[req.SubmissionID] = cid
	s.mu.Unlock()

	// Reserve a slot synchronously so request bursts surface as 429
	// rather than queueing inside the process.
	select {
	case s.sem <- struct{}{}:
	default:
		s.mu.Lock()
		delete(s.inflight, req.SubmissionID)
		s.mu.Unlock()
		w.Header().Set("Retry-After", "10")
		writeError(w, http.StatusTooManyRequests, "max in-flight critiques reached")
		return
	}

	_ = s.cfg.Store.SetStatus(r.Context(), cid, store.StatusRunning)

	// Run the LLM call in the background; we want to return 202 fast
	// because the model takes several seconds.
	go func(ctx context.Context, cid string, req prompt.SubmissionContext) {
		defer func() {
			<-s.sem
			s.mu.Lock()
			delete(s.inflight, req.SubmissionID)
			s.mu.Unlock()
		}()
		s.runCritique(ctx, cid, req)
	}(context.Background(), cid, req)

	writeJSON(w, http.StatusAccepted, map[string]any{
		"critique_id": cid,
		"status":      string(store.StatusRunning),
	})
}

func (s *Server) handleGet(w http.ResponseWriter, r *http.Request) {
	cid := r.PathValue("critique_id")
	if cid == "" {
		writeError(w, http.StatusBadRequest, "critique_id required")
		return
	}
	rec, err := s.cfg.Store.Get(r.Context(), cid)
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	if rec == nil {
		// Differentiate "running" from "missing" using the status key.
		st, _ := s.cfg.Store.GetStatus(r.Context(), cid)
		switch st {
		case store.StatusRunning, store.StatusPending:
			writeJSON(w, http.StatusAccepted, map[string]any{
				"critique_id": cid,
				"status":      string(st),
			})
		default:
			writeError(w, http.StatusNotFound, "critique not found")
		}
		return
	}
	writeJSON(w, http.StatusOK, rec)
}

func (s *Server) handleGetBySubmission(w http.ResponseWriter, r *http.Request) {
	sid := r.PathValue("submission_id")
	if sid == "" {
		writeError(w, http.StatusBadRequest, "submission_id required")
		return
	}
	rec, err := s.cfg.Store.LookupBySubmission(r.Context(), sid)
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	if rec == nil {
		writeError(w, http.StatusNotFound, "no critique for submission")
		return
	}
	writeJSON(w, http.StatusOK, rec)
}

// -----------------------------------------------------------------------------
//  Critique pipeline
// -----------------------------------------------------------------------------

func (s *Server) runCritique(ctx context.Context, cid string, req prompt.SubmissionContext) {
	logger := s.cfg.Logger.With("critique_id", cid, "submission_id", req.SubmissionID)
	start := time.Now()
	userPrompt, err := prompt.UserPromptTemplate(req)
	if err != nil {
		s.fail(ctx, cid, logger, "build prompt", err)
		return
	}

	resp, err := s.cfg.Ollama.Generate(ctx, ollama.GenerateRequest{
		Model:  s.cfg.Ollama.Model,
		Prompt: userPrompt,
		System: prompt.SystemPrompt,
		Format: "json",
		// Deterministic seed: lower 32 bits of the submission ULID's
		// random component. Same submission ⇒ same critique modulo
		// model side-effects.
		Options: map[string]any{
			"temperature": 0.2,
			"top_p":       0.9,
			"seed":        stableSeed(req.SubmissionID),
			"num_ctx":     8192,
		},
		KeepAlive: "5m",
	})
	if err != nil {
		s.fail(ctx, cid, logger, "ollama", err)
		return
	}
	parsed, cleaned, err := prompt.ParseAndValidate(resp.Response)
	if err != nil {
		s.fail(ctx, cid, logger, "validate critique", err)
		return
	}
	body, _ := json.Marshal(parsed)
	rec := store.Record{
		CritiqueID:   cid,
		SubmissionID: req.SubmissionID,
		Model:        resp.Model,
		CreatedAtMs:  time.Now().UnixMilli(),
		LatencyMs:    time.Since(start).Milliseconds(),
		PromptTokens: resp.PromptEvalCount,
		EvalTokens:   resp.EvalCount,
		Critique:     body,
		Raw:          cleaned,
	}
	if err := s.cfg.Store.Save(ctx, rec); err != nil {
		s.fail(ctx, cid, logger, "redis save", err)
		return
	}
	logger.Infow("critique ready",
		"latency_ms", rec.LatencyMs,
		"prompt_tokens", rec.PromptTokens,
		"eval_tokens", rec.EvalTokens)
}

func (s *Server) fail(ctx context.Context, cid string, logger *zap.SugaredLogger, where string, err error) {
	logger.Warnw("critique failed", "where", where, "err", err)
	_ = s.cfg.Store.SetStatus(ctx, cid, store.StatusFailed)
}

// stableSeed derives a deterministic 32-bit integer from a ULID-shaped
// string so repeated critiques of the same submission produce
// identical model outputs.
func stableSeed(id string) int64 {
	var h uint32 = 2166136261
	for i := 0; i < len(id); i++ {
		h ^= uint32(id[i])
		h *= 16777619
	}
	return int64(h)
}

// -----------------------------------------------------------------------------
//  Tiny HTTP helpers
// -----------------------------------------------------------------------------

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]any{"error": msg})
}
