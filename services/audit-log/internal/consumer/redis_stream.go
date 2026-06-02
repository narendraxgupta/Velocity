// Drain audit events from the Redis Stream produced by api-gateway.
//
// The gateway can't speak Kafka cheaply (no librdkafka in our C++
// build), so it XADDs into `audit:events` and we drain into the same
// pipeline as the Kafka path.
//
// Loop:
//   1. XREAD BLOCK 1s COUNT 200 STREAMS audit:events $last
//   2. For each entry, parse `payload`, processRecord(...).
//   3. After the batch, flush QuestDB and advance `$last` only after
//      flush succeeded.
//
// The `$last` cursor is snapshotted to Redis after every successful flush
// (redisCursorKey) so a restart resumes from where it left off instead of
// "$" (newest), which used to silently drop every event emitted during the
// downtime. Resume-from-cursor means events can be redelivered, so
// processEvent dedupes on EventID to keep the hash chain intact.

package consumer

import (
	"context"
	"encoding/json"
	"errors"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/velocity/platform/services/audit-log/internal/event"
)

const (
	redisStreamKey    = "audit:events"
	redisReadCount    = 200
	redisBlockTimeout = 1 * time.Second
	// redisCursorKey persists the last processed stream ID across restarts.
	redisCursorKey = "audit:events:cursor"
	// seenTTL bounds the idempotency markers; far longer than any realistic
	// redelivery window while keeping the key space from growing unbounded.
	seenTTL = 7 * 24 * time.Hour
)

// RunRedisStream is the Redis-side counterpart of Run(). Safe to launch
// in its own goroutine.
func (c *Consumer) RunRedisStream(ctx context.Context) error {
	// Resume from the persisted cursor if present; "$" (only new events)
	// on a cold start with no saved cursor.
	lastID := "$"
	if v, err := c.rdb.Get(ctx, redisCursorKey).Result(); err == nil && v != "" {
		lastID = v
	}
	for {
		select {
		case <-ctx.Done():
			return nil
		default:
		}

		streams, err := c.rdb.XRead(ctx, &redis.XReadArgs{
			Streams: []string{redisStreamKey, lastID},
			Count:   redisReadCount,
			Block:   redisBlockTimeout,
		}).Result()
		if err != nil {
			if errors.Is(err, redis.Nil) || errors.Is(err, context.Canceled) {
				continue
			}
			c.dropped.WithLabelValues("redis_xread").Inc()
			time.Sleep(250 * time.Millisecond)
			continue
		}
		var batchEndID string
		hadMessages := false
		for _, s := range streams {
			for _, m := range s.Messages {
				hadMessages = true
				batchEndID = m.ID
				payload, _ := m.Values["payload"].(string)
				if payload == "" {
					c.dropped.WithLabelValues("redis_no_payload").Inc()
					continue
				}
				var ev event.Event
				if err := json.Unmarshal([]byte(payload), &ev); err != nil {
					c.dropped.WithLabelValues("redis_bad_json").Inc()
					continue
				}
				c.processEvent(ctx, &ev)
			}
		}
		if !hadMessages {
			continue
		}
		if err := c.qdb.FlushNow(); err != nil {
			c.dropped.WithLabelValues("questdb_flush").Inc()
			time.Sleep(250 * time.Millisecond)
			continue
		}
		lastID = batchEndID
		// Snapshot the cursor so a restart resumes here rather than at "$".
		if err := c.rdb.Set(ctx, redisCursorKey, lastID, 0).Err(); err != nil {
			c.log.Warnw("failed to persist audit stream cursor; a restart may replay", "err", err)
		}
	}
}

// processEvent is the shared validate+chain+append path used by both
// the Kafka and Redis sources. Extracted from processRecord so the two
// transports stay independent.
//
// Drop-or-ingest contract: every event is either appended to the writer
// (and counted in `audit_ingested_total`) OR dropped (and counted in
// `audit_drops_total`). Counters are mutually exclusive — alerts on
// drop-rate would otherwise be polluted by partial-failure paths that
// still wrote a row.
func (c *Consumer) processEvent(ctx context.Context, ev *event.Event) {
	if err := ev.Validate(); err != nil {
		c.dropped.WithLabelValues("schema_invalid").Inc()
		return
	}

	// Serialise the whole chain read-modify-write. Both transports feed
	// the same per-tenant chain, so this has to be atomic end-to-end —
	// otherwise the Kafka and Redis goroutines race on the head and two
	// events fork onto the same parent hash.
	c.chainMu.Lock()
	defer c.chainMu.Unlock()

	// Idempotency guard. Resume-from-cursor (Redis) and offset replay
	// (Kafka) can both redeliver an event after a restart; without this a
	// redelivered event would append a duplicate QuestDB row AND fork the
	// per-tenant hash chain. Key on the emitter-supplied ULID (Validate
	// guarantees it's non-empty). The marker is written only AFTER the row
	// is durable and the head advanced (below), so a crash before that
	// leaves the event safely reprocessable. The whole block is under
	// chainMu, so the check+set is atomic w.r.t. the other transport.
	seenKey := "audit:seen:" + ev.EventID
	if n, err := c.rdb.Exists(ctx, seenKey).Result(); err == nil && n > 0 {
		return
	}

	// Pull the chain head. redis.Nil (key missing on first event for a
	// tenant) is the bootstrap case and is NOT a drop reason — `prev`
	// stays empty and ComputeChainHash treats that as the chain root.
	prev, err := c.rdb.Get(ctx, chainKey(ev.TenantID)).Result()
	if err != nil && !errors.Is(err, redis.Nil) {
		c.dropped.WithLabelValues("redis_get").Inc()
		return
	}
	if err := ev.ComputeChainHash(prev); err != nil {
		c.dropped.WithLabelValues("chain_hash").Inc()
		return
	}

	// Durability ordering: the row must be DURABLE before we advance the
	// chain head. Append then force a flush; only if QuestDB accepted the
	// row do we move the head forward. The previous order (advance head,
	// then buffer the row) meant a later flush failure dropped the row but
	// left Redis pointing at its hash — the next event chained onto a hash
	// whose row never landed, and audit-verify (correctly) reported the
	// chain as BROKEN.
	c.qdb.Append(ev)
	if err := c.qdb.FlushNow(); err != nil {
		c.log.Warnw("questdb flush failed; chain head not advanced", "err", err)
		c.dropped.WithLabelValues("questdb_flush").Inc()
		return
	}

	// Row is durable — advance the head. A failure here leaves the head at
	// its previous (still-valid) value; the row is already persisted.
	if err := c.rdb.Set(ctx, chainKey(ev.TenantID),
		ev.ChainHash, 0).Err(); err != nil {
		c.dropped.WithLabelValues("redis_set").Inc()
		return
	}

	// Row is durable and the head advanced — mark the event seen so a
	// redelivery is a no-op rather than a duplicate/chain-fork.
	_ = c.rdb.Set(ctx, seenKey, "1", seenTTL).Err()

	c.ingested.WithLabelValues(ev.TenantID, string(ev.Action)).Inc()
	lag := float64(time.Now().UnixNano()-ev.OccurredAtNs) / 1e6
	if lag > 0 {
		c.lagMs.Observe(lag)
	}
}
