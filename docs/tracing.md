# Distributed Tracing

We propagate W3C Trace Context end-to-end from the public REST request
all the way down to the individual bot worker. Spans are exported via
OTLP/HTTP-JSON to **Jaeger** (the `jaeger:` service in
`infra/compose/services.yml`, profile `debug`).

## Why this matters

Distributed tracing is the difference between "the benchmark started
but no telemetry shows up — somewhere" and "the benchmark started but
no telemetry shows up — the controller never wrote the LoadPlan span
because the worker had no Hello in flight." End-to-end traces collapse
debugging time on a polyglot stack from hours to seconds and make
backpressure issues obvious as bunched-up spans in Jaeger.

## Architecture

```
┌────────────┐   traceparent header     ┌────────────────┐
│  Browser   ├─────────────────────────►│   API Gateway  │
└────────────┘                          │  (Drogon/C++)  │
                                        └────────┬───────┘
                                                 │ gRPC metadata
                                                 │ "traceparent"
                                                 ▼
                                        ┌────────────────┐
                                        │ Bot Controller │
                                        │     (C++)      │
                                        └────────┬───────┘
                                                 │ LoadPlan.traceparent
                                                 ▼
                                        ┌────────────────┐
                                        │  Bot Workers   │
                                        │     (C++)      │
                                        └────────┬───────┘
                                                 │  (per-run span)
                                                 ▼
                                        ┌────────────────┐
                                        │     Jaeger     │
                                        └────────────────┘
```

## Where we hook in

| Layer | Code | What happens |
|---|---|---|
| Gateway | `api-gateway/src/routes/benchmarks.cpp` → `upstream_context()` / `stamp_traceparent()` | Parse incoming `traceparent` header (or mint one), start a span, inject `traceparent` into the outbound gRPC metadata on every controller call. |
| Controller (gRPC server) | `bot-fleet/controller/src/benchmark_service.cpp` → `upstream_context_from()` | Pull `traceparent` from `grpc::ServerContext::client_metadata()`, start a `BenchmarkService.StartBenchmark` span, persist a *fresh child context* in the outgoing `LoadPlan.traceparent`. |
| Worker | `bot-fleet/worker/src/worker.cpp` → `start_run()` | Parse `LoadPlan.traceparent` and open a `bot-worker.run` span for the lifetime of the run. The span closes when the worker tears down. |
| Common | `cmake/common/include/velocity/common/tracing.h` | Header-light W3C context primitives + a lock-free OTLP/HTTP-JSON exporter on a background thread. |

The exporter intentionally does **not** depend on the full
`opentelemetry-cpp` SDK — it's a ~250-line `POST /v1/traces` writer
that talks straight to Jaeger. Avoiding the SDK keeps our build closure
small and our binaries fast.

## Running it locally

```bash
make compose-up                              # apps profile
docker compose --profile debug up -d jaeger  # add the collector
open http://localhost:16686                  # Jaeger UI
```

Then submit a benchmark:

```bash
curl -X POST http://localhost:8080/v1/benchmarks \
  -H 'content-type: application/json' \
  -d '{"submission_id":"01HQEAGIS001","profile":"baseline"}'
```

Within ~2 seconds the trace appears in Jaeger with three (or more)
spans nested under the gateway root. Click any span to see the
`submission_id`, `profile`, and `benchmark_id` attributes.

## What's intentionally *not* traced

- **Per-order spans** — we send hundreds of thousands per second.
  Sampling them would defeat the point; aggregating them is what the
  HdrHistograms in the telemetry-ingester are for.
- **Telemetry / scoring / validator** — these consume from Kafka and
  don't need per-request causal threading. They expose Prometheus
  metrics instead.

## Sampling

Today: always-on, deterministic flag `0x01` set in `traceparent`. The
exporter is designed so a head-based sampler can be dropped in later
behind an env var (`VELOCITY_TRACE_SAMPLE_RATIO`) without touching call
sites. While the platform is still small and developer-facing, "always
trace" is the right default — every benchmark should produce a complete
trace tree.
