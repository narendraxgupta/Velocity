// Package server hosts the gRPC SubmissionService implementation.
//
// Phase 1 lands the lifecycle skeleton: New() builds the server, Serve()
// blocks until ctx is cancelled, Stop() drains. Phase 2 wires in the
// actual RegisterSubmission / Build / Deploy / Teardown handlers backed by
// the builder/, sandbox/, and storage/ packages.
package server

import (
	"context"
	"errors"
	"fmt"
	"net"
	"time"

	"go.uber.org/zap"
	"google.golang.org/grpc"
	"google.golang.org/grpc/health"
	healthpb "google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/keepalive"
	"google.golang.org/grpc/reflection"

	pb "github.com/velocity/platform/proto/gen/go/orchestrator/v1"

	"github.com/velocity/platform/services/submission-engine/internal/builder"
	"github.com/velocity/platform/services/submission-engine/internal/config"
	"github.com/velocity/platform/services/submission-engine/internal/sandbox"
	"github.com/velocity/platform/services/submission-engine/internal/storage"
)

// Deps gathers every external dependency Server needs. Allows tests to
// substitute in-memory implementations.
type Deps struct {
	Storage storage.Storage
	Builder builder.Builder
	Sandbox sandbox.Sandbox
}

// Server is the long-lived process object. Construct via New().
type Server struct {
	cfg    *config.Config
	logger *zap.SugaredLogger
	deps   Deps

	grpcServer *grpc.Server
	listener   net.Listener
}

// New constructs the Server, binds the TCP listener, and prepares the gRPC
// machinery. It does NOT start serving — call Serve() for that.
func New(cfg *config.Config, deps Deps, logger *zap.SugaredLogger) (*Server, error) {
	if deps.Storage == nil || deps.Builder == nil || deps.Sandbox == nil {
		return nil, errors.New("server.New: all deps must be non-nil")
	}

	addr := fmt.Sprintf("0.0.0.0:%d", cfg.GRPCPort)
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", addr, err)
	}

	srv := grpc.NewServer(
		grpc.MaxConcurrentStreams(1024),
		grpc.MaxRecvMsgSize(64*1024*1024),
		grpc.KeepaliveEnforcementPolicy(keepaliveEnforcement),
		grpc.KeepaliveParams(keepaliveParams),
	)

	handlers := NewHandlers(deps.Storage, deps.Builder, deps.Sandbox, logger)
	pb.RegisterSubmissionServiceServer(srv, handlers)

	// Always register the health service so K8s / kube-probes can hit it.
	hsrv := health.NewServer()
	healthpb.RegisterHealthServer(srv, hsrv)
	hsrv.SetServingStatus("", healthpb.HealthCheckResponse_SERVING)
	hsrv.SetServingStatus("velocity.orchestrator.v1.SubmissionService",
		healthpb.HealthCheckResponse_SERVING)

	reflection.Register(srv)

	return &Server{
		cfg:        cfg,
		logger:     logger,
		deps:       deps,
		grpcServer: srv,
		listener:   lis,
	}, nil
}

// Serve blocks until ctx is cancelled, at which point it returns.
func (s *Server) Serve(ctx context.Context) error {
	errCh := make(chan error, 1)

	go func() {
		s.logger.Infow("gRPC ready", "addr", s.listener.Addr().String())
		errCh <- s.grpcServer.Serve(s.listener)
	}()

	select {
	case <-ctx.Done():
		s.logger.Infow("server context cancelled")
		return ctx.Err()
	case err := <-errCh:
		if err != nil && !errors.Is(err, grpc.ErrServerStopped) {
			return fmt.Errorf("grpc serve: %w", err)
		}
		return nil
	}
}

// Stop drains existing connections and stops accepting new ones. Honors the
// timeout in `ctx` — if the graceful path exceeds it, falls back to a hard
// stop.
func (s *Server) Stop(ctx context.Context) error {
	doneCh := make(chan struct{})
	go func() {
		s.grpcServer.GracefulStop()
		close(doneCh)
	}()

	select {
	case <-doneCh:
		return nil
	case <-ctx.Done():
		s.logger.Warnw("graceful shutdown timed out; forcing")
		s.grpcServer.Stop()
		return ctx.Err()
	}
}

// Keepalive defaults — let API gateway and bot controller maintain long-lived
// streams without being killed for "ping floods" or idle expiry.
var (
	keepaliveEnforcement = keepalive.EnforcementPolicy{
		MinTime:             10 * time.Second,
		PermitWithoutStream: true,
	}
	keepaliveParams = keepalive.ServerParameters{
		MaxConnectionIdle: 0, // never close idle conns
		Time:              60 * time.Second,
		Timeout:           20 * time.Second,
	}
)
