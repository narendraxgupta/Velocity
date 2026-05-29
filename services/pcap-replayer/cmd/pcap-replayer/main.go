// Command pcap-replayer turns a recorded pcap (produced by pcap-recorder
// during an earlier benchmark) back into a live TCP byte stream fired at a
// target submission endpoint.
//
// Two use-cases drive the design:
//
//  1. Bug bisect — "engine v2 fails on commit X but not Y; replay commit
//     X's pcap against commit Y to isolate the regression." This requires
//     *byte-for-byte deterministic replay* so any divergence between runs
//     is attributable to the engine, not the bot fleet.
//
//  2. Production replay — operator captures real venue traffic during a
//     live event, replays against a candidate engine in pre-prod. This
//     requires preserving inter-packet timing (PCAP_CLOCK_MODE_PRESERVE)
//     so the engine sees realistic temporal microstructure.
//
// HTTP API (the api-gateway proxies through to these; PcapService gRPC
// lands in a future iteration once buf-regen settles):
//
//	POST /v1/pcaps/replay
//	     { target_submission_id, target_host, target_port,
//	       source_object_key, clock_mode, fixed_rps, speed_multiplier }
//	     → starts replay; returns { benchmark_id, expected_packets }
//
//	GET  /v1/pcaps/{benchmark_id}/state
//	     → polls live replay progress (sent, errored, current_rps)
//
//	GET  /v1/pcaps
//	     → enumerates available pcap objects in MinIO `pcaps/` prefix
//
//	GET  /healthz
package main

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"syscall"

	"github.com/redis/go-redis/v9"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/pcap-replayer/internal/replayer"
	"github.com/velocity/platform/services/pcap-replayer/internal/server"
	"github.com/velocity/platform/services/pcap-replayer/internal/store"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "pcap-replayer: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	logger, err := zap.NewProduction()
	if err != nil {
		return fmt.Errorf("zap: %w", err)
	}
	sugar := logger.Sugar()

	httpAddr := envOr("REPLAYER_HTTP_ADDR", "0.0.0.0:8092")
	bucket := envOr("MINIO_BUCKET", "velocity-artefacts")
	minioEndpoint := envOr("MINIO_ENDPOINT", "minio.velocity-control.svc.cluster.local:9000")
	minioAccess := envOr("MINIO_ACCESS_KEY", "minio")
	minioSecret := envOr("MINIO_SECRET_KEY", "minio123")
	redisAddr := envOr("REDIS_ADDR", "redis.velocity-control.svc.cluster.local:6379")

	st, err := store.NewMinIO(minioEndpoint, minioAccess, minioSecret, bucket, false)
	if err != nil {
		return fmt.Errorf("minio: %w", err)
	}

	rdb := redis.NewClient(&redis.Options{Addr: redisAddr})
	defer func() { _ = rdb.Close() }()

	rep := replayer.New(replayer.Deps{
		Logger:  sugar,
		Storage: st,
		Redis:   rdb,
	})

	srv := server.New(server.Config{
		Addr:     httpAddr,
		Logger:   sugar,
		Replayer: rep,
		Storage:  st,
	})

	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	errCh := make(chan error, 1)
	go func() {
		sugar.Infow("HTTP listening", "addr", httpAddr, "bucket", bucket)
		errCh <- srv.Listen()
	}()

	select {
	case <-ctx.Done():
		sugar.Infow("shutdown requested")
		rep.CancelAll()
		return srv.Shutdown(context.Background())
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			return fmt.Errorf("http server: %w", err)
		}
		return nil
	}
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
