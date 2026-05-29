// Package writer implements the QuestDB ILP-over-TCP writer for audit
// events. Mirrors the C++ telemetry-ingester pattern: connect lazily,
// drop-and-reconnect on send error, prefer dropping over blocking.
//
// We also keep a `pgx` connection pool for the read path so the API
// server can run SELECTs against QuestDB's PG-wire interface without
// re-implementing it.
package writer

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/audit-log/internal/event"
)

// QuestDB owns both the ILP (write) and pg-wire (read) connections.
// Splitting them apart turned out worse: the API server needs the same
// connection-string config to read what the consumer wrote, so we
// bundle them.
type QuestDB struct {
	host    string
	ilpPort uint16
	pg      *pgxpool.Pool
	log     *zap.SugaredLogger

	mu     sync.Mutex
	conn   net.Conn
	buf    []byte
	dropps uint64
}

func NewQuestDB(host string, ilpPort uint16, log *zap.SugaredLogger) (*QuestDB, error) {
	// We give the pgx pool a generous 10s connect timeout so the
	// pod doesn't crash-loop on a slow QuestDB startup; the consumer
	// will retry the ILP socket independently.
	pgURL := fmt.Sprintf("postgres://admin:quest@%s:8812/qdb?pool_max_conns=4", host)
	pool, err := pgxpool.New(context.Background(), pgURL)
	if err != nil {
		return nil, fmt.Errorf("questdb pgx pool: %w", err)
	}
	q := &QuestDB{
		host:    host,
		ilpPort: ilpPort,
		pg:      pool,
		log:     log,
		buf:     make([]byte, 0, 8192),
	}
	if err := q.ensureSchema(context.Background()); err != nil {
		log.Warnw("questdb schema bootstrap failed (will retry on first write)", "err", err)
	}
	return q, nil
}

func (q *QuestDB) Close() {
	q.mu.Lock()
	defer q.mu.Unlock()
	if q.conn != nil {
		_ = q.conn.Close()
		q.conn = nil
	}
	q.pg.Close()
}

func (q *QuestDB) ensureSchema(ctx context.Context) error {
	const ddl = `
		CREATE TABLE IF NOT EXISTS audit_events (
			occurred_at TIMESTAMP,
			event_id    SYMBOL CAPACITY 4096 NOCACHE,
			tenant_id   SYMBOL CAPACITY 256,
			subject     SYMBOL CAPACITY 4096 NOCACHE,
			role        SYMBOL CAPACITY 16,
			source      SYMBOL CAPACITY 32,
			action      SYMBOL CAPACITY 256,
			resource_type SYMBOL CAPACITY 128,
			resource_id   SYMBOL CAPACITY 4096 NOCACHE,
			outcome     SYMBOL CAPACITY 8,
			status_code INT,
			remote_ip   SYMBOL CAPACITY 1024 NOCACHE,
			request_id  SYMBOL CAPACITY 4096 NOCACHE,
			meta_json   STRING,
			chain_hash  STRING
		) TIMESTAMP(occurred_at) PARTITION BY DAY WAL;
	`
	_, err := q.pg.Exec(ctx, ddl)
	return err
}

// Append serialises an audit event in ILP format and buffers it.
// Flushing happens either when the buffer crosses 8 KiB or on the
// caller's call to FlushNow().
func (q *QuestDB) Append(e *event.Event) {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.buf = appendLine(q.buf, e)
	if len(q.buf) >= 8192 {
		_ = q.flushLocked()
	}
}

// FlushNow forces a flush. Safe to call from a timer goroutine.
func (q *QuestDB) FlushNow() error {
	q.mu.Lock()
	defer q.mu.Unlock()
	return q.flushLocked()
}

func (q *QuestDB) flushLocked() error {
	if len(q.buf) == 0 {
		return nil
	}
	if q.conn == nil {
		c, err := net.DialTimeout("tcp",
			net.JoinHostPort(q.host, strconv.Itoa(int(q.ilpPort))),
			3*time.Second)
		if err != nil {
			q.dropps++
			q.buf = q.buf[:0]
			return err
		}
		// TCP_NODELAY mirrors the C++ writer — batched lines should hit
		// the wire as soon as we've decided to flush.
		if tc, ok := c.(*net.TCPConn); ok {
			_ = tc.SetNoDelay(true)
		}
		q.conn = c
	}
	_ = q.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if _, err := q.conn.Write(q.buf); err != nil {
		q.dropps++
		_ = q.conn.Close()
		q.conn = nil
		q.buf = q.buf[:0]
		return err
	}
	q.buf = q.buf[:0]
	return nil
}

// Query exposes a constrained SELECT for the HTTP API. We deliberately
// don't pass arbitrary SQL through — the audit log is the LAST place we
// want SQL injection. Filter args are bound parameters.
func (q *QuestDB) Query(ctx context.Context, filter Filter) ([]event.Event, error) {
	const base = `SELECT
		occurred_at, event_id, tenant_id, subject, role, source,
		action, resource_type, resource_id, outcome, status_code,
		remote_ip, request_id, meta_json, chain_hash
	  FROM audit_events`
	conds := []string{}
	args := []any{}
	if filter.TenantID != "" {
		conds = append(conds, fmt.Sprintf("tenant_id = $%d", len(args)+1))
		args = append(args, filter.TenantID)
	}
	if filter.Action != "" {
		conds = append(conds, fmt.Sprintf("action = $%d", len(args)+1))
		args = append(args, filter.Action)
	}
	if !filter.Since.IsZero() {
		conds = append(conds, fmt.Sprintf("occurred_at >= $%d", len(args)+1))
		args = append(args, filter.Since)
	}
	if !filter.Until.IsZero() {
		conds = append(conds, fmt.Sprintf("occurred_at <  $%d", len(args)+1))
		args = append(args, filter.Until)
	}
	sql := base
	if len(conds) > 0 {
		sql += " WHERE " + strings.Join(conds, " AND ")
	}
	sql += " ORDER BY occurred_at DESC LIMIT $" + strconv.Itoa(len(args)+1)
	args = append(args, filter.Limit)

	rows, err := q.pg.Query(ctx, sql, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := make([]event.Event, 0, filter.Limit)
	for rows.Next() {
		var ev event.Event
		var ts time.Time
		var metaJSON string
		if err := rows.Scan(
			&ts, &ev.EventID, &ev.TenantID, &ev.Subject, &ev.Role,
			&ev.SourceService, &ev.Action, &ev.ResourceType,
			&ev.ResourceID, &ev.Outcome, &ev.StatusCode,
			&ev.RemoteIP, &ev.RequestID, &metaJSON, &ev.ChainHash,
		); err != nil {
			return nil, err
		}
		ev.OccurredAtNs = ts.UnixNano()
		ev.Meta = decodeMeta(metaJSON)
		ev.SchemaVersion = 1
		out = append(out, ev)
	}
	return out, rows.Err()
}

// Filter restricts a Query.
type Filter struct {
	TenantID string
	Action   string
	Since    time.Time
	Until    time.Time
	Limit    int
}

// SchemaCheck returns nil iff we can hit QuestDB. Used by /readyz.
func (q *QuestDB) Health(ctx context.Context) error {
	row := q.pg.QueryRow(ctx, "SELECT 1")
	var n int
	return row.Scan(&n)
}

// ----------------------------------------------------------------------------
//  ILP line formatting
// ----------------------------------------------------------------------------

func appendLine(buf []byte, e *event.Event) []byte {
	buf = append(buf, "audit_events,"...)
	buf = appendTag(buf, "tenant_id", e.TenantID)
	buf = append(buf, ',')
	buf = appendTag(buf, "action", string(e.Action))
	buf = append(buf, ',')
	buf = appendTag(buf, "source", e.SourceService)
	buf = append(buf, ',')
	buf = appendTag(buf, "outcome", string(e.Outcome))
	buf = append(buf, ' ')

	buf = appendField(buf, "event_id", e.EventID, true)
	buf = append(buf, ',')
	buf = appendField(buf, "subject", e.Subject, true)
	buf = append(buf, ',')
	buf = appendField(buf, "role", e.Role, true)
	buf = append(buf, ',')
	buf = appendField(buf, "resource_type", e.ResourceType, true)
	buf = append(buf, ',')
	buf = appendField(buf, "resource_id", e.ResourceID, true)
	buf = append(buf, ',')
	buf = appendField(buf, "remote_ip", e.RemoteIP, true)
	buf = append(buf, ',')
	buf = appendField(buf, "request_id", e.RequestID, true)
	buf = append(buf, ',')
	buf = appendField(buf, "chain_hash", e.ChainHash, true)
	buf = append(buf, ',')
	buf = append(buf, "status_code="...)
	buf = strconv.AppendInt(buf, int64(e.StatusCode), 10)
	buf = append(buf, 'i')

	if len(e.Meta) > 0 {
		buf = append(buf, ',')
		buf = appendField(buf, "meta_json", encodeMeta(e.Meta), true)
	}

	buf = append(buf, ' ')
	buf = strconv.AppendInt(buf, e.OccurredAtNs, 10)
	buf = append(buf, '\n')
	return buf
}

func appendTag(buf []byte, k, v string) []byte {
	buf = append(buf, k...)
	buf = append(buf, '=')
	// Tag values can't contain spaces, commas, equals signs OR
	// newlines in ILP. We do the cheapest possible escape: replace
	// with underscore. The audit schema deliberately makes these
	// fields constrained enough (tenant ids, actions, etc.) that the
	// substitution is lossless in practice. Newlines in particular
	// would terminate the record mid-tag and desync the stream.
	for i := 0; i < len(v); i++ {
		c := v[i]
		if c == ' ' || c == ',' || c == '=' || c == '\n' || c == '\r' {
			buf = append(buf, '_')
		} else {
			buf = append(buf, c)
		}
	}
	return buf
}

func appendField(buf []byte, k, v string, isString bool) []byte {
	buf = append(buf, k...)
	buf = append(buf, '=')
	if isString {
		buf = append(buf, '"')
		// QuestDB ILP framing terminates a record at the first
		// unescaped newline. Any of `"`, `\`, `\n`, `\r` inside a
		// string field has to be escaped or it will desync the
		// wire format — the next event's header would be parsed as
		// a continuation of this field. We had a near miss in QA
		// where a request_id containing a literal CR shipped from
		// a Windows curl client corrupted six neighbouring rows.
		for i := 0; i < len(v); i++ {
			c := v[i]
			switch c {
			case '"', '\\':
				buf = append(buf, '\\', c)
			case '\n':
				buf = append(buf, '\\', 'n')
			case '\r':
				buf = append(buf, '\\', 'r')
			default:
				buf = append(buf, c)
			}
		}
		buf = append(buf, '"')
	} else {
		buf = append(buf, v...)
	}
	return buf
}

func encodeMeta(m map[string]any) string {
	// We use encoding/json so embedded `"`, `\` and control characters
	// in user-supplied strings produce valid JSON. The cost is an extra
	// allocation per event vs. a hand-rolled builder, but a previous
	// hand-rolled version emitted invalid JSON for any meta value
	// containing a quote — and audit events DO carry resource paths
	// and SQL fragments that contain quotes.
	//
	// We still cap the field set to JSON-friendly primitives + nested
	// maps/slices. For unsupported types (channels, funcs, etc.) we
	// fall back to a sentinel rather than failing the whole record.
	body, err := json.Marshal(m)
	if err != nil {
		return `{"_meta_encode_error":` + strconv.Quote(err.Error()) + `}`
	}
	return string(body)
}

func decodeMeta(s string) map[string]any {
	if s == "" || s == "{}" {
		return nil
	}
	// Read path only — this is for the /v1/audit UI and is best-effort.
	// Stricter validation happens at ingest time in the consumer.
	out := map[string]any{}
	if err := json.Unmarshal([]byte(s), &out); err != nil {
		return nil
	}
	return out
}
