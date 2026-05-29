// Package api implements the read-side of the audit log: GET /v1/audit
// with bounded SQL parameters, JSON output. Mounted under the audit-log
// daemon's HTTP server and proxied through the api-gateway.
//
// Why not let the gateway hit QuestDB directly? Two reasons:
//   1. We want the chain-hash decoded once, server-side, so clients
//      can't accidentally shuffle rows and recompute a "valid" chain.
//   2. Putting the SQL boundary in a Go service keeps the gateway's
//      C++ surface free of pgx, which would be a brittle dep.
package api

import (
	"context"
	"encoding/json"
	"net/http"
	"strconv"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/audit-log/internal/writer"
)

type Server struct {
	addr string
	qdb  *writer.QuestDB
	reg  *prometheus.Registry
	log  *zap.SugaredLogger
}

func NewServer(addr string, qdb *writer.QuestDB, reg *prometheus.Registry,
	log *zap.SugaredLogger) *Server {
	return &Server{addr: addr, qdb: qdb, reg: reg, log: log}
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/audit", s.handleQuery)
	mux.HandleFunc("/readyz", s.handleReady)
	return mux
}

// GET /v1/audit?tenant=…&action=…&since=RFC3339&until=RFC3339&limit=N
func (s *Server) handleQuery(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	q := r.URL.Query()
	filter := writer.Filter{
		TenantID: q.Get("tenant"),
		Action:   q.Get("action"),
		Limit:    parseLimit(q.Get("limit")),
	}
	if v := q.Get("since"); v != "" {
		t, err := time.Parse(time.RFC3339, v)
		if err != nil {
			http.Error(w, "bad since", http.StatusBadRequest)
			return
		}
		filter.Since = t
	}
	if v := q.Get("until"); v != "" {
		t, err := time.Parse(time.RFC3339, v)
		if err != nil {
			http.Error(w, "bad until", http.StatusBadRequest)
			return
		}
		filter.Until = t
	}

	ctx, cancel := contextWithTimeout(r, 10*time.Second)
	defer cancel()

	events, err := s.qdb.Query(ctx, filter)
	if err != nil {
		s.log.Warnw("audit query failed", "err", err)
		http.Error(w, "query failed", http.StatusBadGateway)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"events": events,
		"count":  len(events),
	})
}

func (s *Server) handleReady(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := contextWithTimeout(r, 2*time.Second)
	defer cancel()
	if err := s.qdb.Health(ctx); err != nil {
		http.Error(w, "questdb down", http.StatusServiceUnavailable)
		return
	}
	_, _ = w.Write([]byte("ok\n"))
}

func parseLimit(s string) int {
	const def = 100
	const max = 5_000
	if s == "" {
		return def
	}
	n, err := strconv.Atoi(s)
	if err != nil || n <= 0 {
		return def
	}
	if n > max {
		return max
	}
	return n
}

// contextWithTimeout is split out so future server middleware (auth,
// rate limit) can chain into it without each handler reimplementing
// the request-context dance.
func contextWithTimeout(r *http.Request, d time.Duration) (context.Context, context.CancelFunc) {
	return context.WithTimeout(r.Context(), d)
}
