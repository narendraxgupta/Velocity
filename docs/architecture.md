# Velocity — Architecture Blueprint

This document is the **single source of truth** for how Velocity is structured.
It is written for engineers who will operate, extend, or audit the platform —
not for marketing. Every claim in here is implemented in code; every diagram
matches the running system.

---

## 1. Goals & Non-Goals

### Goals

1. **Securely host arbitrary trading code** uploaded as submissions.
   No process running in our cluster may break out of its sandbox.
2. **Generate adversarial load** that is *honest* — millions of orders per
   second, with measurements that respect Coordinated Omission and produce
   defensible tail-latency numbers.
3. **Validate correctness** of every fill against a reference matching
   engine that we control. Price-time priority violations are detected and
   penalized.
4. **Score and rank** submissions in real time, with sub-second update
   latency from order placement to leaderboard movement.
5. **Be reproducible**: identical inputs → identical scores. Achieved
   through deterministic RNG seeds, pinned CPUs, isolated network paths.

### Non-Goals

- **Real money or real exchange connectivity.** The "FIX" we support is FIX 4.4
  over a non-routed dev network, with synthetic instrument symbols.
- **Multi-tenant production isolation guarantees.** We isolate untrusted code
  via gVisor + cgroups but do not promise SOC 2 or PCI-DSS compliance.
- **General-purpose load testing.** Velocity is opinionated about the trading
  domain: orderbook semantics, FIX message types, price-time priority. It is
  not a drop-in replacement for `wrk` or `k6`.

---

## 2. System Context

```mermaid
flowchart LR
    Submitter([Submitter / Engineer])
    Operator([Platform Operator])
    Viewer([Read-only Viewer])

    subgraph velocity [Velocity Platform]
        Web["Web UI (Next.js)"]
        API["API Gateway (C++)"]
        Engine["Submission Engine (Go)"]
        Sandbox["Sandbox (gVisor + cgroups)"]
        Bots["Bot Fleet (C++ + io_uring)"]
        Pipe["Telemetry + Validator (C++)"]
        Score["Scoring + Leaderboard (C++)"]
    end

    External1[("Object Storage<br/>MinIO / S3")]
    External2[("Container Registry")]

    Submitter -- "upload binary / source" --> Web
    Web --> API
    Operator -- "operate / tune" --> Web
    Viewer -- "observe live" --> Web

    API --> Engine --> Sandbox
    API --> Bots
    Bots --> Sandbox
    Bots --> Pipe --> Score --> Web
    Engine --> External2
    Engine --> External1
```

---

## 3. Component Catalogue

| Component | Language | Lines (target) | Runtime resources | Hot-path role |
|-----------|----------|----------------|-------------------|---------------|
| `api-gateway` | C++20 / Drogon | ~2k | 2 vCPU / 512MB | Yes — public HTTP/WS ingress |
| `submission-engine` | Go 1.22 | ~3k | 1 vCPU / 256MB | No — control plane |
| `bot-fleet/controller` | C++20 / gRPC | ~1.5k | 2 vCPU / 256MB | No — fan-out only |
| `bot-fleet/worker` | C++20 / io_uring / uWS | ~4k | N vCPU / 1GB each | **Yes** — load generator |
| `telemetry-ingester` | C++20 / librdkafka | ~2.5k | 2 vCPU / 1GB | **Yes** — consumes events |
| `correctness-validator` | C++20 / Boost.Intrusive | ~3k | 2 vCPU / 2GB | Yes — replays orderbook |
| `leaderboard-ws` | C++20 / uWebSockets | ~1k | 1 vCPU / 256MB | Yes — broadcast |
| `frontend` | TS / Next.js 14 | ~3k | 1 vCPU / 512MB | No |

### Sandbox guests (submission code)

Constrained per submission:

- **CPU**: 2 dedicated cores via `cpu-manager-policy=static`.
- **Memory**: 512 MiB hard cap (OOM-killed if exceeded).
- **Ephemeral storage**: 1 GiB tmpfs at `/tmp`.
- **Network**: ingress only from `velocity-bot-fleet/*`, no egress.
- **Filesystem**: read-only root, `tmpfs` for `/tmp` and `/var/log`.
- **Lifetime**: 600 s wall clock — defense in depth.

---

## 4. Data Flows

### 4.1 Submission lifecycle

```mermaid
sequenceDiagram
    autonumber
    participant U as Submitter
    participant W as Web UI
    participant A as API Gateway
    participant M as MinIO
    participant S as Submission Engine
    participant K as Kaniko
    participant R as Registry
    participant V as gVisor Pod

    U->>W: drop .tar.gz / Dockerfile
    W->>A: POST /v1/submissions  (multipart)
    A->>M: PutObject(artefacts/<sub_id>.tar.gz)
    A->>S: gRPC Register{sub_id, sha256, key}
    S-->>A: SubmissionId
    A-->>W: 201 Created + WS subscription
    par Build (async)
        S->>K: build OCI image
        K->>R: docker push registry:5000/velocity/<sub_id>
        K-->>S: image_ref
    end
    S->>V: KubeAPI: create Pod (gVisor runtimeClass)
    V-->>S: PodReady
    S-->>A: SubmissionStatus{HEALTHY, endpoint}
    A-->>W: WS push HEALTHY
```

### 4.2 Benchmark execution

```mermaid
sequenceDiagram
    autonumber
    participant A as API Gateway
    participant C as Bot Controller
    participant W as Bot Worker(s)
    participant V as Sandbox Pod
    participant K as Redpanda
    participant I as Telemetry Ingester
    participant CV as Correctness Validator
    participant Q as QuestDB
    participant R as Redis
    participant LB as Leaderboard WS

    A->>C: StartBenchmark{sub_id, profile}
    C->>W: stream LoadPlan
    loop driven at intended-send-time
        W->>V: order N
        V-->>W: ack N
        W->>K: publish OrderEvent (batched)
    end
    par Telemetry
        K->>I: consume OrderEvent[]
        I->>I: HdrHistogram::recordValue
        I->>Q: ILP UDP append (latency_us, …)
        I->>R: ZADD leaderboard score
        R-->>LB: PUBLISH leaderboard.updates
        LB-->>A: WS push (relayed)
    and Correctness
        K->>CV: consume OrderEvent[]
        CV->>CV: replay reference orderbook
        CV->>Q: ILP UDP append (fills, violations)
        CV->>R: ZADD correctness:<sub_id>
    end
```

### 4.3 Live UI subscription

```mermaid
sequenceDiagram
    participant B as Browser
    participant A as API Gateway
    participant L as Leaderboard WS
    participant R as Redis

    B->>L: WS GET /ws (subscribe leaderboard)
    L->>R: SUBSCRIBE leaderboard.updates
    R-->>L: pub message
    L-->>B: LeaderboardDelta frame
    B->>A: HTTP GET /v1/benchmarks/<id>/snapshot
    A-->>B: BenchmarkSnapshot
    B->>A: WS /v1/benchmarks/<id>/stream
    A-->>B: BenchmarkSnapshot frames @ 4Hz
```

---

## 5. Cross-Cutting Concerns

### 5.1 Time

- **Source of truth**: `clock_gettime(CLOCK_MONOTONIC_RAW)` on every host.
- **Wallclock correlation**: at process start each service captures
  `(MONOTONIC_RAW, REALTIME)` once and stores the offset. All published
  timestamps are MONOTONIC values translated through this offset.
- **NTP slewing**: ignored on purpose. `MONOTONIC_RAW` is immune.
- **Cross-host comparison**: only the bot worker measures order latency.
  Cross-host time skew never enters a latency calculation.

### 5.2 Concurrency

- **C++ services** use a **per-core thread model**, pinned with
  `pthread_setaffinity_np`. Each thread runs its own io_uring (worker) or
  event loop (API gateway), with **shared state via SPSC ring buffers**
  (no shared mutexes on the hot path).
- **Cross-service** ordering is provided by Redpanda partitions keyed on
  `submission_id` — all events for one submission land on one partition,
  consumed by one ingester worker. Sequential consistency per submission.

### 5.3 Backpressure

- **Bot → Redpanda**: bounded local buffer (default 64k events). On overflow,
  events are *dropped with metric* rather than blocking the io_uring loop.
  The metric (`velocity_telemetry_dropped_total`) is a benchmark-quality
  signal — a high drop rate indicates we are bottlenecked on ourselves and
  must scale workers.
- **Redpanda → Ingester**: standard consumer group, lag visible in
  Grafana. Consumer scales horizontally; partition count is the ceiling.
- **Validator → QuestDB**: ILP over UDP is fire-and-forget. We accept
  occasional packet loss in exchange for never blocking the validator's hot
  path. QuestDB's WAL recovers gracefully from gaps.

### 5.4 Failure modes & isolation

| Failure | Detection | Response |
|---------|-----------|----------|
| Submission code crashes | K8s pod CrashLoopBackoff | Mark submission `CRASHED`; cancel running benchmark; -50 penalty |
| Submission OOM | OOMKilled event | Mark `OOM_KILLED`; -25 penalty; teardown |
| Bot worker dies | gRPC stream EOF | Controller rebalances load across survivors; benchmark continues with reduced RPS noted in report |
| Ingester lag | Consumer group lag > 5s | Page on call; benchmark may continue, scores degrade gracefully |
| Validator slow | Stage timeout | Validator restarts with checkpoint; only correctness score affected |
| Redpanda broker down | health probe | (Production) leader re-election. (Dev) single-broker, no recovery — restart compose |
| Redis down | health probe | Leaderboard stops updating; reads serve from in-memory cache in leaderboard-ws |

### 5.5 Observability

Three pillars:

- **Metrics** — Prometheus via `prometheus-cpp` in every service. Histograms
  use `HdrHistogram` and are exposed via a custom Prometheus exporter that
  encodes the full HDR payload as a buckets vector.
- **Traces** — A small hand-written W3C trace-context library with an
  OTLP/HTTP batch exporter to Jaeger. Every benchmark carries a
  `traceparent` end-to-end so a run can be followed
  gateway → controller → worker → ingester → leaderboard.
- **Logs** — Structured JSON via `spdlog` with a custom JSON sink.
  Stdout/stderr → Docker JSON log driver → (production) Loki.

---

## 6. Deployment Topology

### 6.1 Local development (Docker Compose)

Single host, single Redpanda broker, single QuestDB node. See
[`docker-compose.yml`](../docker-compose.yml).

### 6.2 Production (Kubernetes)

```mermaid
flowchart TB
    subgraph k8s [Kubernetes Cluster]
        subgraph nsControl [namespace: velocity-control]
            APIGW[api-gateway × 3]
            SE[submission-engine × 2]
            LBWS[leaderboard-ws × 3]
        end
        subgraph nsLoad [namespace: velocity-load]
            BC[bot-controller × 1]
            BW[bot-worker × 8-32]
        end
        subgraph nsData [namespace: velocity-data]
            Ing[telemetry-ingester × 4]
            CV[correctness-validator × 2]
        end
        subgraph nsSandbox [namespace: velocity-sandbox]
            S1[Sandbox Pod 1]
            S2[Sandbox Pod 2]
            Sn[Sandbox Pod N]
        end
        subgraph nsInfra [namespace: velocity-infra]
            Rp[Redpanda × 3]
            Q[QuestDB StatefulSet]
            Rd[Redis Sentinel × 3]
            M[MinIO × 4]
        end
    end

    APIGW --> SE --> S1
    BC --> BW --> S1
    BW --> Rp --> Ing --> Q
    Ing --> Rd --> LBWS
```

Key K8s-native pieces:

- **`RuntimeClass: gvisor`** on every sandbox pod.
- **`PodSecurityContext`** with `runAsNonRoot`, `readOnlyRootFilesystem`,
  `allowPrivilegeEscalation: false`, dropped capabilities.
- **`NetworkPolicy`** restricting sandbox ingress to `velocity-load` and
  blocking all egress.
- **`ResourceQuota`** per namespace.
- **`PriorityClass`** — bots > ingester > frontend, so we shed UI updates
  before we shed measurements.

See [`infra/kubernetes/`](../infra/kubernetes/) for the manifests.

---

## 7. Security Model

Threat → mitigation:

| Threat | Mitigation |
|--------|------------|
| Submission escapes sandbox to host | gVisor user-space syscall interception |
| Submission exhausts host resources | cgroups v2 hard caps; lifetime watchdog |
| Submission scans private network | egress-deny NetworkPolicy; no DNS resolver |
| Submission exploits kernel CVE | gVisor's `runsc` rewrites syscalls in Go user-space |
| Submission exploits gVisor | `seccomp-bpf` baseline + dropped capabilities |
| Submitter supplies malicious image | Image is built *from source* in Kaniko; we never run an OCI image they provide directly |
| Operator privilege escalation | RBAC; submission-engine has only Pod create/delete in `velocity-sandbox` |

---

## 8. Build & Release

- **C++**: CMake + Conan 2.x. Locked dependency versions in
  [`conanfile.py`](../conanfile.py).
- **Go**: standard module workflow, vendored in production.
- **Frontend**: Next.js with `output: 'standalone'`, deployed as a static
  bundle behind nginx in K8s.
- **Containers**: multi-stage builds, distroless runtime images, non-root
  user, immutable image tags (`{service}:{git-sha}-{build-no}`).
- **CI** (future): GitHub Actions matrix — build → test → lint → push.
- **CD** (future): Argo CD watching the `infra/kubernetes/` overlays.

---

## 9. Architecture Decision Records (ADRs)

The decisions that *make* this architecture, with their alternatives and
rationale, are in [`adr/`](adr/).

- [ADR-001 — gVisor over Firecracker](adr/001-gvisor-over-firecracker.md)
- [ADR-002 — C++ on the hot path](adr/002-cpp-on-the-hot-path.md)
- [ADR-003 — QuestDB over TimescaleDB](adr/003-questdb-over-timescale.md)
- [ADR-004 — Coordinated Omission correction](adr/004-coordinated-omission.md)
- [ADR-005 — Redpanda over Kafka](adr/005-redpanda-over-kafka.md)

---

## 10. Glossary

- **Bot persona** — A trading strategy a bot worker simulates
  (Market Maker, Aggressive Taker, Canceller, Spoofer, Noise).
- **Coordinated Omission** — The class of latency-measurement bugs where
  a slow server makes the load generator wait, biasing tail latency
  measurements low. Coined by Gil Tene.
- **HdrHistogram** — High-Dynamic-Range Histogram. A bucket scheme that
  records values across N orders of magnitude with constant relative
  precision. Mergeable across processes.
- **ILP** — InfluxDB Line Protocol; QuestDB's fastest write path.
- **Price-time priority** — The rule that orders at the best price fill
  first, and within a price level the oldest order fills first.
- **Sandbox** — A gVisor-isolated Kubernetes Pod running an uploaded submission.
