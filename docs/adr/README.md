# Architecture Decision Records

Decisions that shape Velocity's structure, recorded in the
[Michael Nygard format](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions).

| # | Title | Status |
|---|-------|--------|
| [001](001-gvisor-over-firecracker.md) | gVisor over Firecracker for sandboxing | Accepted |
| [002](002-cpp-on-the-hot-path.md) | C++ on the measurement hot path | Accepted |
| [003](003-questdb-over-timescale.md) | QuestDB over TimescaleDB for telemetry storage | Accepted |
| [004](004-coordinated-omission.md) | Open-loop load with intended-send-time correction | Accepted |
| [005](005-redpanda-over-kafka.md) | Redpanda over Apache Kafka | Accepted |

## When to add an ADR

Add one when:

- A choice will be hard to reverse later (storage engine, RPC framework).
- The choice has at least one *credible* alternative that an outsider would
  reasonably propose.
- You expect to be asked "why did you do it this way?" by a reviewer or a
  future maintainer (likely future-you).

## Template

Copy from [`_template.md`](_template.md).
