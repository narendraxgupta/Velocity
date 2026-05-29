// =============================================================================
//  audit-verify — offline integrity checker for the audit log.
//
//  Walks the audit_events table in occurred_at order (per tenant) and
//  recomputes chain_hash = SHA-256(prev_hash || canonical_json(row)).
//  Any mismatch is reported with the offending event_id and line number.
//
//  Usage:
//
//    audit-verify --questdb-host questdb.velocity-data.svc:8812 \
//                 --tenant acme \
//                 --since 2026-05-19T00:00:00Z
//
//  Exit codes:
//    0 — chain intact
//    1 — chain BROKEN; offending events printed
//    2 — query / connectivity error
// =============================================================================

package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/velocity/platform/services/audit-log/internal/event"
)

func main() {
	host := flag.String("questdb-host", "questdb.velocity-data.svc.cluster.local",
		"QuestDB host (PG-wire)")
	port := flag.Int("questdb-port", 8812, "QuestDB pg-wire port")
	tenant := flag.String("tenant", "", "Tenant id (required)")
	sinceStr := flag.String("since", "",
		"Only check events at or after this RFC3339 timestamp")
	flag.Parse()

	if *tenant == "" {
		fmt.Fprintln(os.Stderr, "audit-verify: --tenant is required")
		os.Exit(2)
	}

	url := fmt.Sprintf("postgres://admin:quest@%s:%d/qdb", *host, *port)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	pool, err := pgxpool.New(ctx, url)
	if err != nil {
		fmt.Fprintln(os.Stderr, "questdb connect:", err)
		os.Exit(2)
	}
	defer pool.Close()

	sql := `SELECT
		occurred_at, event_id, tenant_id, subject, role, source,
		action, resource_type, resource_id, outcome, status_code,
		remote_ip, request_id, meta_json, chain_hash
	  FROM audit_events
	  WHERE tenant_id = $1`
	args := []any{*tenant}
	if *sinceStr != "" {
		t, err := time.Parse(time.RFC3339, *sinceStr)
		if err != nil {
			fmt.Fprintln(os.Stderr, "bad --since:", err)
			os.Exit(2)
		}
		sql += " AND occurred_at >= $2"
		args = append(args, t)
	}
	sql += " ORDER BY occurred_at ASC"

	rows, err := pool.Query(ctx, sql, args...)
	if err != nil {
		fmt.Fprintln(os.Stderr, "query:", err)
		os.Exit(2)
	}
	defer rows.Close()

	prev := ""
	broken := 0
	checked := 0

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
			fmt.Fprintln(os.Stderr, "scan:", err)
			os.Exit(2)
		}
		ev.OccurredAtNs = ts.UnixNano()
		ev.SchemaVersion = 1
		if metaJSON != "" && metaJSON != "{}" {
			_ = json.Unmarshal([]byte(metaJSON), &ev.Meta)
		}
		stored := ev.ChainHash
		if err := ev.ComputeChainHash(prev); err != nil {
			fmt.Fprintln(os.Stderr, "rehash:", err)
			os.Exit(2)
		}
		if stored != ev.ChainHash {
			broken++
			fmt.Printf("BROKEN  %s  event_id=%s  stored=%s  recomputed=%s\n",
				ts.Format(time.RFC3339Nano), ev.EventID,
				short(stored), short(ev.ChainHash))
		}
		prev = ev.ChainHash
		checked++
	}
	if err := rows.Err(); err != nil {
		fmt.Fprintln(os.Stderr, "rows:", err)
		os.Exit(2)
	}

	fmt.Printf("checked=%d broken=%d\n", checked, broken)
	if broken > 0 {
		os.Exit(1)
	}
}

func short(s string) string {
	if len(s) <= 12 {
		return s
	}
	return s[:8] + "…" + s[len(s)-4:]
}
