// =============================================================================
//  audit-log — daemon entrypoint.
//
//  Three goroutines:
//    1. Kafka consumer pulling audit events off `t.<tenant>.audit` topics.
//    2. QuestDB writer (ILP over TCP) flushing batches every 250ms or
//       8 KiB, whichever is sooner.
//    3. HTTP server hosting /v1/audit query API + /metrics + /healthz.
//
//  Hash-chaining
//  -------------
//  Each row carries a `chain_hash` column computed as
//      SHA-256(prev_hash || canonical_json(this_row_without_chain_hash))
//  We keep the chain head in Redis (one key per tenant) so we don't have
//  to round-trip QuestDB on each insert. Chain validation is a SELECT-
//  in-order + replay; tooling for that lives in cmd/audit-verify.
//
//  Operator note: dropping rows is preferable to blocking the consumer.
//  We track drops in `audit_drops_total{reason="..."}` so an alert can
//  fire if a misconfigured pipeline starts losing more than 0.1% of
//  events.
// =============================================================================
package main

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	_ "net/http/pprof"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/redis/go-redis/v9"
	"github.com/twmb/franz-go/pkg/kgo"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/audit-log/internal/api"
	"github.com/velocity/platform/services/audit-log/internal/consumer"
	"github.com/velocity/platform/services/audit-log/internal/writer"
)

func main() {
	logger, _ := zap.NewProduction()
	defer func() { _ = logger.Sync() }()
	sugar := logger.Sugar()

	cfg := loadConfig()
	sugar.Infow("audit-log starting", "config", cfg)

	rdb := redis.NewClient(&redis.Options{Addr: cfg.RedisAddr})
	defer rdb.Close()

	qdb, err := writer.NewQuestDB(cfg.QuestDBHost, cfg.QuestDBILPPort, sugar)
	if err != nil {
		sugar.Fatalw("questdb writer init failed", "err", err)
	}
	defer qdb.Close()

	// Bring up Kafka consumer. We subscribe to a pattern so newly-
	// provisioned tenants are picked up without a restart.
	cli, err := kgo.NewClient(
		kgo.SeedBrokers(strings.Split(cfg.KafkaBrokers, ",")...),
		kgo.ConsumerGroup(cfg.KafkaGroup),
		kgo.ConsumeRegex(),
		kgo.ConsumeTopics(`t\.[a-z0-9-]+\.audit`),
		kgo.DisableAutoCommit(), // we commit AFTER successful flush
		kgo.SessionTimeout(30*time.Second),
	)
	if err != nil {
		sugar.Fatalw("kafka client init failed", "err", err)
	}
	defer cli.Close()

	registry := prometheus.NewRegistry()
	cons := consumer.New(cli, qdb, rdb, sugar, registry)

	httpSrv := api.NewServer(cfg.HTTPListen, qdb, registry, sugar)

	ctx, cancel := signal.NotifyContext(context.Background(),
		os.Interrupt, syscall.SIGTERM)
	defer cancel()

	httpReady := make(chan struct{})
	go func() {
		mux := http.NewServeMux()
		mux.Handle("/metrics", promhttp.HandlerFor(registry, promhttp.HandlerOpts{}))
		mux.Handle("/", httpSrv.Handler())
		mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte("ok\n"))
		})
		close(httpReady)
		s := &http.Server{Addr: cfg.HTTPListen, Handler: mux,
			ReadHeaderTimeout: 5 * time.Second}
		if err := s.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			sugar.Errorw("http server stopped", "err", err)
		}
	}()
	<-httpReady

	// Redis stream draining runs in parallel with Kafka consumption.
	// The two share the same QuestDB writer and chain-hashing logic, so
	// the order of events across transports is "as observed by us" —
	// good enough since audit events carry their own occurred_at_ns
	// and the chain is per-tenant.
	go func() {
		if err := cons.RunRedisStream(ctx); err != nil {
			sugar.Errorw("redis stream consumer exited", "err", err)
		}
	}()

	if err := cons.Run(ctx); err != nil {
		sugar.Errorw("consumer exited", "err", err)
	}
	sugar.Info("audit-log shutting down")
}

type config struct {
	RedisAddr      string
	QuestDBHost    string
	QuestDBILPPort uint16
	KafkaBrokers   string
	KafkaGroup     string
	HTTPListen     string
}

func loadConfig() config {
	get := func(k, def string) string {
		if v := os.Getenv(k); v != "" {
			return v
		}
		return def
	}
	geti := func(k string, def int) int {
		if v := os.Getenv(k); v != "" {
			if i, err := strconv.Atoi(v); err == nil {
				return i
			}
		}
		return def
	}
	return config{
		RedisAddr:      get("REDIS_ADDR", "redis.velocity-data.svc.cluster.local:6379"),
		QuestDBHost:    get("QUESTDB_HOST", "questdb.velocity-data.svc.cluster.local"),
		QuestDBILPPort: uint16(geti("QUESTDB_ILP_PORT", 9009)),
		KafkaBrokers:   get("KAFKA_BROKERS", "redpanda.velocity-data.svc.cluster.local:9092"),
		KafkaGroup:     get("KAFKA_GROUP", "audit-log"),
		HTTPListen:     fmt.Sprintf(":%d", geti("HTTP_PORT", 8081)),
	}
}
