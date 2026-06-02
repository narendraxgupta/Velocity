# ADR-002: C++ on the measurement hot path

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: Platform team

## Context

The platform's headline metrics — p99 latency, peak sustained TPS,
correctness — are only as honest as the code measuring them.

A load generator written in a GC'd language has two problems:

1. **GC pauses are observable.** If the bot worker pauses for 5 ms during a
   benchmark, every order in flight at that moment registers an inflated
   latency. p99 is contaminated.
2. **Allocator overhead is observable.** Allocating a protobuf message per
   order at 1M orders/sec means *millions* of `malloc` calls per second.
   `malloc` has tail latency (it can take a slow path) which leaks into the
   measurement.

A correctness validator that is slower than the data stream falls behind, so
its results are no longer "real time" — the leaderboard lies until the
validator catches up.

The choice of language for these components is therefore not a matter of
taste. It is a matter of measurement quality.

## Decision

Every service on the **measurement hot path** is written in **C++20**:

- `bot-fleet/worker` — the load generator
- `bot-fleet/controller` — fan-out
- `telemetry-ingester` — Redpanda → HdrHistogram → QuestDB
- `correctness-validator` — reference orderbook replay
- `api-gateway` — public HTTP/WS ingress
- `leaderboard-ws` — WebSocket broadcast

The **non-hot-path** services are written in **Go**, because they orchestrate
Docker/K8s APIs where Go has first-class library support and writing them in
C++ has no perf benefit:

- `submission-engine` — talks to Docker SDK, K8s `client-go`, MinIO

## Consequences

### Good

- **Zero GC pauses** on the hot path. Latency measurements reflect the
  submission's behaviour, not our internal scheduling.
- **Allocator control** — protobuf arena allocation in the worker keeps the
  publish path allocation-free per message after warm-up. (Swapping the system
  allocator for `tcmalloc`/`jemalloc` is a drop-in tuning step if profiling
  calls for it; it is not wired into the build today.)
- **`io_uring` is available** with first-class Linux syscall access — Go's
  `net` package would need significant CGo glue.
- **`pthread_setaffinity_np` + `sched_setscheduler(SCHED_FIFO)`** are
  trivial in C++, painful in Go.
- **Domain alignment** — submissions tend to be C++/Rust/Go themselves.
  Velocity speaking C++ on the hot path signals fluency in the
  ecosystem of the systems it benchmarks.

### Bad

- **Slower development** vs Go for the same line count — roughly 1.5–2×
  on average. We mitigated by keeping the **non-hot-path** services in
  Go (saves ~3000 lines of orchestration code).
- **Build complexity** — Conan + CMake adds setup cost. We invested in a
  shared `cmake/` module so individual services are short.
- **Library quality varies** — for example, `librdkafka` is rock solid but
  some C++ wrappers are not; we wrote a thin wrapper of our own where
  needed.

## Alternatives considered

### Rust everywhere

- **Pro**: Comparable performance, memory safety, modern tooling, async/await
  ergonomics.
- **Con**: Existing fluency is significantly stronger in C++ here, and the
  ramp-up cost on Rust's borrow checker for io_uring + custom allocators
  is not worth paying right now.
- We could revisit this for v2; the architecture is language-agnostic at the
  RPC boundaries.

### Go everywhere (including hot path)

- **Pro**: One language, fastest development velocity.
- **Pro**: Goroutines scale to 100k+ trivially on a single host.
- **Con**: GC pauses contaminate p99 measurements. Documented in production
  reports from Discord, CockroachDB, others.
- **Con**: No equivalent to `io_uring` integration without significant
  effort.

### Hybrid: Rust for bots, Go for everything else

- **Pro**: Memory safety, comparable perf, modern.
- **Con**: Same ramp-up cost as all-Rust.

### Hybrid: C++ for bots only, Go for everything else

- This is closer to what we landed on, but moving the ingester and
  validator off the hot path's runtime guarantees would mean the
  same GC-pause problem in those services. We kept them in C++ for
  measurement quality, not pure speed.

## References

- [Coordinated Omission talk (Gil Tene)](https://www.youtube.com/watch?v=lJ8ydIuPFeU)
- [Discord: Why Discord is switching from Go to Rust](https://discord.com/blog/why-discord-is-switching-from-go-to-rust)
- [`io_uring` overview (Jens Axboe)](https://kernel.dk/io_uring.pdf)
