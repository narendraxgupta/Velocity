// Package consumer plumbs Kafka audit events into the QuestDB writer.
//
// Run-loop shape:
//
//   1. Poll Kafka for up to ~100 records.
//   2. For each record:
//        a. Unmarshal JSON → Event, validate.
//        b. Pull the chain head from Redis (key per tenant).
//        c. Compute chain hash, set it on the event.
//        d. Atomically update Redis with the new head.
//        e. Append the event to the QuestDB writer.
//   3. FlushNow() at the end of each batch, then commit Kafka offsets.
//      Commit AFTER flush so we never ACK an event we lost.
//
// Failure modes are spelled out at each step — most are "drop the
// record, increment a counter, keep going" because a permanent backlog
// would defeat the purpose of an audit log.
package consumer

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/redis/go-redis/v9"
	"github.com/twmb/franz-go/pkg/kgo"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/audit-log/internal/event"
	"github.com/velocity/platform/services/audit-log/internal/writer"
)

type Consumer struct {
	cli   *kgo.Client
	qdb   *writer.QuestDB
	rdb   *redis.Client
	log   *zap.SugaredLogger
	flush time.Duration

	// chainMu serialises the read-modify-write of the per-tenant chain
	// head. Run() (Kafka) and RunRedisStream() (Redis) call processEvent
	// from two goroutines against the SAME tenant keyspace; without this
	// lock the Get→hash→Set sequence races and two events can chain onto
	// the same parent, silently forking a supposedly tamper-evident chain.
	chainMu sync.Mutex

	ingested *prometheus.CounterVec
	dropped  *prometheus.CounterVec
	lagMs    prometheus.Histogram
}

func New(cli *kgo.Client, qdb *writer.QuestDB, rdb *redis.Client,
	log *zap.SugaredLogger, reg prometheus.Registerer) *Consumer {

	ingested := prometheus.NewCounterVec(
		prometheus.CounterOpts{
			Name: "audit_ingested_total",
			Help: "Number of audit events written to QuestDB.",
		},
		[]string{"tenant", "action"},
	)
	dropped := prometheus.NewCounterVec(
		prometheus.CounterOpts{
			Name: "audit_drops_total",
			Help: "Audit events dropped, by reason.",
		},
		[]string{"reason"},
	)
	lag := prometheus.NewHistogram(prometheus.HistogramOpts{
		Name:    "audit_ingest_lag_ms",
		Help:    "Wall-clock latency from event.occurred_at to ingest.",
		Buckets: []float64{1, 5, 25, 100, 500, 1_000, 5_000, 25_000, 60_000},
	})
	reg.MustRegister(ingested, dropped, lag)

	return &Consumer{
		cli:      cli,
		qdb:      qdb,
		rdb:      rdb,
		log:      log,
		flush:    250 * time.Millisecond,
		ingested: ingested,
		dropped:  dropped,
		lagMs:    lag,
	}
}

// Run blocks until ctx is cancelled, processing audit events. Returns
// nil on clean shutdown, error only for unrecoverable upstream
// failures.
func (c *Consumer) Run(ctx context.Context) error {
	timer := time.NewTicker(c.flush)
	defer timer.Stop()

	for {
		select {
		case <-ctx.Done():
			_ = c.qdb.FlushNow()
			return nil
		default:
		}

		fetches := c.cli.PollFetches(ctx)
		if errs := fetches.Errors(); len(errs) > 0 {
			for _, e := range errs {
				if errors.Is(e.Err, context.Canceled) {
					continue
				}
				c.log.Warnw("kafka fetch error", "topic", e.Topic, "err", e.Err)
			}
		}

		iter := fetches.RecordIter()
		processed := 0
		for !iter.Done() {
			rec := iter.Next()
			c.processRecord(ctx, rec)
			processed++
		}
		if processed > 0 {
			if err := c.qdb.FlushNow(); err != nil {
				c.log.Warnw("questdb flush failed; offsets not committed", "err", err)
				continue
			}
			if err := c.cli.CommitUncommittedOffsets(ctx); err != nil {
				c.log.Warnw("kafka commit failed", "err", err)
			}
		}

		select {
		case <-timer.C:
			_ = c.qdb.FlushNow()
		default:
		}
	}
}

func (c *Consumer) processRecord(ctx context.Context, rec *kgo.Record) {
	var ev event.Event
	if err := json.Unmarshal(rec.Value, &ev); err != nil {
		c.dropped.WithLabelValues("invalid_json").Inc()
		return
	}
	c.processEvent(ctx, &ev)
}

func chainKey(tenant string) string {
	return fmt.Sprintf("audit:chain:head:%s", tenant)
}
