# ADR-003: QuestDB over TimescaleDB for telemetry storage

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: Platform team

## Context

Each benchmark produces tens of millions of `OrderEvent`s. The storage layer
must:

1. **Ingest at line rate.** During a peak benchmark we push ≥1M rows/sec
   sustained.
2. **Support time-window aggregations** — p50/p90/p99 over the last 5s, TPS
   per second, fill counts per minute.
3. **Be queryable from SQL** so the frontend can ship arbitrary analytical
   queries without us writing a custom API for each chart.
4. **Run in a single container in dev** for fast inner-loop iteration.

The "standard" answer in 2026 is **TimescaleDB**. It is excellent. We need to
explain why we are not using it.

## Decision

We use **QuestDB 8.x** as the primary telemetry store. ILP (InfluxDB Line
Protocol) over **TCP** is the write path; the Postgres wire protocol is the
read path. (ILP/UDP was removed upstream in QuestDB 7+; TCP is the only
supported on-wire ILP transport for current releases.)

## Consequences

### Good

- **ILP TCP writes** at multi-million rows/sec on a single node with batched
  writes — well beyond TimescaleDB's ~1M rows/sec ceiling on similar hardware.
- **Native microsecond timestamps** — TimescaleDB stores `timestamp(6)`
  microseconds but `timestamptz` is the canonical type, and many client
  libraries truncate to milliseconds without warning.
- **SQL surface is Postgres-compatible enough** for our queries: `WHERE
  ts > now() - 5s`, `SAMPLE BY 1s`, `LATEST ON ts PARTITION BY symbol`. We
  do *not* depend on Postgres-specific features.
- **Single-binary deployment** in dev, single StatefulSet in prod.
- **Schema-on-the-fly** via ILP — adding a new tag column is just including
  it in the next write. Useful in an evolving project where the telemetry
  schema is still being discovered.

### Bad

- **No transactions** — QuestDB is append-mostly. We never `UPDATE` rows,
  so this is fine. If we did, we would not be using it.
- **Smaller ecosystem** vs Postgres/TimescaleDB. Some tooling (e.g., Grafana
  dashboards templated from common metrics) requires a small mapping
  layer.
- **Compaction can stutter** under sustained heavy load. Mitigated by sizing
  `cairo.max.uncommitted.rows` per `infra/compose/config/questdb-server.conf`.

## Alternatives considered

### TimescaleDB

- **Pro**: Real Postgres. `percentile_cont` is built-in and battle-tested.
  Familiar to every engineer.
- **Pro**: Continuous aggregates make 5-second rollups cheap.
- **Con**: ~3–4× slower ILP-equivalent write path under our workload (we
  benchmarked `pg_copy` and the `postgres_fdw` ingest pattern; QuestDB ILP
  beats both significantly).
- **Con**: Hypertable chunk management adds operational complexity that
  exceeds our current storage needs for benchmark telemetry.

### ClickHouse

- **Pro**: Best-in-class analytical queries; columnar; outscales QuestDB on
  reads for complex queries.
- **Pro**: Native HDR-histogram-compatible state (`quantilesState`).
- **Con**: Heavier operational footprint; the write path (Kafka engine or
  buffer tables) is more brittle than QuestDB ILP for our pattern.
- **Con**: Steeper learning curve for ad-hoc queries.

### Influx (OSS 3.0 / Cloud)

- **Pro**: Native ILP. Mature ecosystem.
- **Con**: 3.0 is Apache DataFusion under the hood — fine, but at the time
  of writing its OSS distribution is in flux. Operational risk.

### Plain Postgres with hypertable-style partitioning written by hand

- **Pro**: No new dependency.
- **Con**: We would reinvent TimescaleDB poorly.

## Migration path

If QuestDB becomes a problem (e.g., scaling beyond one node, more complex
schema needs), the migration to TimescaleDB or ClickHouse is straightforward:
the ingester writes line-protocol that both products understand, and our
queries are SQL. We have intentionally avoided QuestDB-specific syntax (no
`LATEST ON … PARTITION BY` in critical paths).

## References

- [QuestDB ILP performance docs](https://questdb.io/docs/reference/api/ilp/)
- [TimescaleDB hypertable docs](https://docs.timescale.com/use-timescale/latest/hypertables/)
- Internal benchmark: `scripts/bench-tsdb/README.md` (see `proto/results.csv`)
