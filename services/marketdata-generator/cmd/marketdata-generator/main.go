// Command marketdata-generator drives a stochastic reference price feed.
//
// The binary supports two operating modes:
//   - Standalone: spawns the generator with a default 3-symbol
//     correlated portfolio (SPOT/PERP/FUTURES) and publishes to Redis.
//   - Configured: env vars override the symbol list and parameters.
//
// Configuration via env vars (all optional):
//
//	MARKETDATA_TICK_MS          → tick interval in ms (default 10)
//	MARKETDATA_REDIS_ADDR       → Redis address (default redis:6379)
//	MARKETDATA_KAFKA_BROKERS    → comma-list, empty disables Kafka publish
//	MARKETDATA_KAFKA_TOPIC      → defaults to "marketdata.ticks"
//	MARKETDATA_SEED             → deterministic seed, 0 = wall-clock
//	MARKETDATA_HTTP_ADDR        → HTTP introspection bind, default :8093
package main

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/twmb/franz-go/pkg/kgo"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/marketdata-generator/internal/generator"
)

func main() {
	logger, _ := zap.NewProduction()
	defer logger.Sync()
	sug := logger.Sugar()

	tickMs, _ := strconv.Atoi(env("MARKETDATA_TICK_MS", "10"))
	if tickMs <= 0 {
		tickMs = 10
	}
	seed, _ := strconv.ParseInt(env("MARKETDATA_SEED", "0"), 10, 64)

	rdb := redis.NewClient(&redis.Options{
		Addr: env("MARKETDATA_REDIS_ADDR", "redis:6379"),
	})
	if err := rdb.Ping(context.Background()).Err(); err != nil {
		sug.Warnw("redis ping failed (proceeding without Redis publish)", "err", err)
		rdb = nil
	}

	var kClient *kgo.Client
	if brokers := env("MARKETDATA_KAFKA_BROKERS", ""); brokers != "" {
		opts := []kgo.Opt{
			kgo.SeedBrokers(strings.Split(brokers, ",")...),
			kgo.ProducerLinger(5 * time.Millisecond),
			kgo.MaxBufferedRecords(64 * 1024),
		}
		var err error
		kClient, err = kgo.NewClient(opts...)
		if err != nil {
			sug.Warnw("kafka client init failed (proceeding without Kafka publish)",
				"err", err)
		}
	}

	// Default portfolio: SPOT / PERP / FUTURES with realistic basis
	// spreads and correlated Wiener innovations.
	symbols := []generator.SymbolConfig{
		{
			Symbol: "SPOT/USDT", InitialPrice: 100.0, Theta: 0.50, Mu: 100.0,
			Sigma: 0.20, JumpRate: 0.003, JumpMean: 0.10, JumpStddev: 0.05,
			PriceScale: 1_000_000,
		},
		{
			Symbol: "BTC-PERP", InitialPrice: 100.05, Theta: 0.45, Mu: 100.05,
			Sigma: 0.22, JumpRate: 0.005, JumpMean: 0.12, JumpStddev: 0.06,
			PriceScale: 1_000_000,
		},
		{
			Symbol: "BTC-DEC25", InitialPrice: 99.95, Theta: 0.40, Mu: 99.95,
			Sigma: 0.25, JumpRate: 0.002, JumpMean: 0.08, JumpStddev: 0.04,
			PriceScale: 1_000_000,
		},
	}
	correlations := [][]float64{
		{1.00, 0.95, 0.85},
		{0.95, 1.00, 0.88},
		{0.85, 0.88, 1.00},
	}

	gen, err := generator.New(generator.Config{
		TickInterval: time.Duration(tickMs) * time.Millisecond,
		Symbols:      symbols,
		Correlations: correlations,
		Redis:        rdb,
		Kafka:        kClient,
		KafkaTopic:   env("MARKETDATA_KAFKA_TOPIC", "marketdata.ticks"),
		Logger:       sug,
		Seed:         seed,
	})
	if err != nil {
		sug.Fatalw("generator init failed", "err", err)
	}

	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGTERM, syscall.SIGINT)
	defer cancel()

	// HTTP introspection: GET /snapshot returns current prices.
	mux := http.NewServeMux()
	mux.HandleFunc("GET /snapshot", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(gen.Snapshot())
	})
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	})

	srv := &http.Server{
		Addr:              env("MARKETDATA_HTTP_ADDR", ":8093"),
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		sug.Infow("HTTP introspection bound", "addr", srv.Addr)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			sug.Warnw("HTTP serve error", "err", err)
		}
	}()
	go func() {
		<-ctx.Done()
		shutdownCtx, c := context.WithTimeout(context.Background(), 5*time.Second)
		defer c()
		_ = srv.Shutdown(shutdownCtx)
	}()

	if err := gen.Run(ctx); err != nil {
		sug.Fatalw("generator failed", "err", err)
	}
}

func env(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
