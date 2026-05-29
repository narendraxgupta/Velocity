# Velocity — Setup & Run Guide

> Step-by-step procedure to set up the Velocity benchmarking platform on a
> fresh laptop and run it end-to-end.
>
> Everything in this document is derived from the actual files in this repo
> (`docker-compose.yml`, `infra/compose/services.yml`, `Makefile`,
> `scripts/*.ps1`, `conanfile.py`, `frontend/package.json`,
> `infra/helm/velocity/values.yaml`). It is not generic boilerplate.

> **Prefer the cloud?** To skip a local Docker install entirely, push the repo
> and open it in **GitHub Codespaces** — the dev container brings up Linux +
> Docker-in-Docker so the whole stack (including the Linux-only C++ services)
> builds and runs in the browser/VS Code. See
> [`../.devcontainer/README.md`](../.devcontainer/README.md). The guide below
> covers the local-laptop path.

---

## Table of contents

1. [What this project is (so the steps make sense)](#1-what-this-project-is)
2. [Hardware & OS prerequisites](#2-hardware--os-prerequisites)
3. [Software to install on the host](#3-software-to-install-on-the-host)
4. [Get the code onto the new laptop](#4-get-the-code-onto-the-new-laptop)
5. [One-time bootstrap](#5-one-time-bootstrap)
6. [Configure environment variables](#6-configure-environment-variables)
7. [Start the stack](#7-start-the-stack)
8. [Verify it works (end-to-end smoke test)](#8-verify-it-works)
9. [Day-to-day commands](#9-day-to-day-commands)
10. [Frontend-only development (no Docker rebuilds)](#10-frontend-only-development)
11. [SDK builds (optional)](#11-sdk-builds)
12. [Production / Kubernetes deployment](#12-production--kubernetes-deployment)
13. [Troubleshooting](#13-troubleshooting)
14. [One-page TL;DR](#14-one-page-tldr)

---

## 1. What this project is

**Velocity** is a multi-language distributed system. It is *not* an app
you `npm install && npm start`. It is composed of:

| Layer | What runs there | Language |
|---|---|---|
| **Infrastructure** (always-on) | Redpanda (Kafka), QuestDB (time-series), Redis, MinIO (S3), Prometheus, Grafana, Jaeger, container registry | Third-party images |
| **Application services** (`apps` profile) | api-gateway, submission-engine, bot-controller, bot-worker, telemetry-ingester, correctness-validator, scoring-service, leaderboard-ws, audit-log, anomaly-detector, marketdata-generator, critique-service, ollama, frontend | C++20, Go 1.22+, Python, TypeScript |
| **Frontend** | Next.js 14 leaderboard UI | TypeScript / React |
| **SDKs** (optional, for users of the platform) | `sdk/go`, `sdk/ts`, `sdk/python` | Go, TS, Python |

The design choice that drives the entire setup is this:
**C++ services depend on `io_uring`, `liburing`, gVisor,
`pthread_setaffinity_np`, and `CLOCK_MONOTONIC_RAW` — none of which exist
on Windows or macOS natively.** So every build and every service runs
inside Linux containers via Docker. Your laptop only needs Docker and a
couple of host tools; it does *not* need a local C++ toolchain, Conan,
Go, or Python.

---

## 2. Hardware & OS prerequisites

Confirmed against `scripts/bootstrap.ps1`:

- **OS:** Windows 10/11 (PowerShell), macOS 13+, or any modern Linux.
  The repo ships PowerShell wrappers (`dev-up.ps1`, `bootstrap.ps1`)
  **and** a Makefile, so both work.
- **RAM:** 16 GB minimum, 32 GB strongly recommended. Docker Desktop
  must be allocated **at least 12 GB**, ideally 16 GB. The C++ build
  alone (Conan + multiple service images) will OOM at 8 GB.
- **CPU:** any x86_64 with virtualization enabled in BIOS
  (VT-x / AMD-V). Apple Silicon works but the bot-worker `io_uring`
  path requires Rosetta-free Linux containers.
- **Disk:** **30 GB+ free** for Docker volumes and images
  (Redpanda + QuestDB + MinIO + base images alone are ~6 GB; the C++
  builder image is ~3 GB).
- **WSL2** on Windows (required by Docker Desktop's Linux engine).

---

## 3. Software to install on the host

Only these are mandatory on the host. Everything else runs inside
containers.

### Mandatory

| Tool | Version | Why |
|---|---|---|
| **Docker Desktop** | 4.30+ (engine 24+) | Runs every service, every build. Must be on the **Linux engine**, not Windows containers. |
| **Git** | any recent | Cloning. On Windows install **Git for Windows** — it gives you `bash` + `curl` + `jq`-style core tools used by `scripts/e2e-smoke.sh`. |
| **PowerShell** | 5.1+ (built into Windows) **or** GNU `make` + `bash` | Either run `.\scripts\dev-up.ps1` or `make`. The PS1 wrapper exists specifically because Windows users often don't have `make`. |

### Optional (only for local dev *outside* Docker)

| Tool | Version | Why |
|---|---|---|
| **Node.js** | 20 LTS+ (`frontend/package.json` declares `engines.node >=20`) | Run `npm run dev` against the frontend without rebuilding the Docker image on every change. |
| **Go** | 1.22+ | Hack on `services/submission-engine` or `services/plugin-orchestrator` with fast `go run`. |
| **Python** | 3.11+ | Build `sdk/python` or hack on `services/anomaly-detector`. |
| **jq, curl, tar, sha256sum** | latest | Required by `scripts/e2e-smoke.sh`. On Windows they ship with Git for Windows. |
| **Helm 3** + **kubectl** | latest | Only if you want to deploy to Kubernetes (skip for dev). |

### Configure Docker Desktop (one-time)

1. Open Docker Desktop → **Settings**.
2. **Resources → Advanced**: CPUs ≥ 4, Memory ≥ 12 GB, Swap ≥ 1 GB,
   Disk image size ≥ 64 GB.
3. **General**: enable *"Use the WSL 2 based engine"* (Windows only).
4. **Resources → WSL Integration**: enable for your default WSL distro
   (Windows only).
5. Restart Docker Desktop. Run `docker info` — it must print without
   error.

---

## 4. Get the code onto the new laptop

```powershell
# Pick a path WITHOUT spaces and ideally NOT inside OneDrive.
# OneDrive's file watcher and "placeholder" files cause grief with
# Docker bind mounts, Node file watchers, and TS-server caching.
# Recommended: C:\dev\velocity   (or ~/dev/velocity on macOS/Linux)
cd C:\dev
git clone <your-remote-url> velocity
cd velocity\platform
```

If you have to keep it inside OneDrive, **right-click the `platform`
folder → "Always keep on this device"** so OneDrive stops virtualizing
the files. (This is the same trick that prevents intermittent
TS-server "Cannot find module" caching glitches.)

---

## 5. One-time bootstrap

This pulls every base image and generates protobuf stubs once. It is
idempotent — safe to re-run.

### Windows (PowerShell)

```powershell
.\scripts\bootstrap.ps1
```

### Linux / macOS / WSL / Git Bash

```bash
make bootstrap
```

What this actually does (from `scripts/bootstrap.ps1` lines 58–94):

1. Verifies `docker` and `git` are installed and the Docker daemon is
   responding.
2. `docker compose pull --ignore-pull-failures` → downloads Redpanda,
   QuestDB, Redis, MinIO, Prometheus, Grafana, Jaeger, Ollama, the
   registry, and `bufbuild/buf`. **First run downloads ~6 GB; budget
   10–20 minutes on a decent connection.**
3. `docker run --rm bufbuild/buf:1.45.0 generate` (inside `proto/`) →
   produces the C++/Go/TS gRPC + protobuf stubs into `proto/gen/`.
   These are gitignored and must be regenerated on every fresh clone.

Expected final lines:

```
[OK] Bootstrap complete.
```

---

## 6. Configure environment variables

Only the frontend needs a `.env.local`. The backend services get their
config from `infra/compose/services.yml` (already filled in for local
dev).

```powershell
cd frontend
Copy-Item .env.example .env.local
cd ..
```

Default values from `.env.example` (do not change for local dev):

- `NEXT_PUBLIC_API_GATEWAY_URL=http://localhost:8080`
- `NEXT_PUBLIC_LEADERBOARD_WS=ws://localhost:8090/v1/leaderboard`
- `NEXT_PUBLIC_JAEGER_URL=http://localhost:16686`
- `NEXT_PUBLIC_VELOCITY_ENV=dev`
- `NEXT_PUBLIC_DEMO_MODE=0` (set to `1` if you want to demo the UI
  without bringing up the backend — every hook then serves canned data
  from `frontend/lib/demo-data.ts`)

There are **no secrets to provision** for local dev. MinIO uses
`velocity / velocity-dev-secret`, Grafana uses `velocity / velocity-dev`,
QuestDB uses `velocity / velocity-dev`. All hardcoded in
`docker-compose.yml`. They are dev credentials only — never deploy this
compose file to production.

---

## 7. Start the stack

There are **two profiles** and you pick depending on what you want to
do.

### Option A — Infra only (fast, for backend/SDK dev)

Brings up Redpanda, QuestDB, Redis, MinIO, registry, Prometheus,
Grafana, Jaeger, Redpanda Console. No application services. Use this
when you'll run an app service locally (e.g.
`go run ./services/submission-engine`).

```powershell
.\scripts\dev-up.ps1 up
# or:  make up
```

After ~30 seconds you'll see:

```
✓ infra ready
  Redpanda Console : http://localhost:8085
  QuestDB Console  : http://localhost:9000
  MinIO Console    : http://localhost:9101 (velocity / velocity-dev-secret)
  Grafana          : http://localhost:3001 (velocity / velocity-dev)
  Prometheus       : http://localhost:9090
```

### Option B — Full platform (infra + every app service + frontend)

This is what you want to demo end-to-end. The first run **builds every
service image from source** (~15–30 minutes on a cold cache).

```powershell
# 1. Build all service images. Slow first time, fast on rebuild thanks
#    to layer cache.
docker compose --profile apps build

# 2. Start infra + apps together.
.\scripts\dev-up.ps1 up-apps
# or:  make up-apps
```

You'll see:

```
✓ platform running
  Frontend         : http://localhost:3000
  API Gateway      : http://localhost:8080
  Leaderboard WS   : ws://localhost:8090/v1/leaderboard
```

### What's actually running (from `infra/compose/services.yml`)

| Service | Port | Notes |
|---|---|---|
| `frontend` (Next.js) | **3000** | `GET /` |
| `api-gateway` (C++/Drogon) | **8080** | `GET /healthz` |
| `leaderboard-ws` (C++/uWebSockets) | **8090** | `ws://…/v1/leaderboard` |
| `submission-engine` (Go) | 7001 (gRPC) | — |
| `bot-controller` (C++/gRPC) | 7002 (gRPC) | — |
| `bot-worker` (C++/io_uring) | scaled via `--scale bot-worker=N` | — |
| `telemetry-ingester` (C++) | — | consumes Redpanda → writes QuestDB |
| `correctness-validator` (C++) | — | reference orderbook replay |
| `scoring-service` (C++) | — | joins streams → Redis ZSET |
| `audit-log` (Go) | 8081 | tamper-evident hash chain |
| `anomaly-detector` (Python) | 8095 | isolation-forest on metrics |
| `marketdata-generator` (Go) | 8093 | synthetic ticks |
| `critique-service` (Go + Ollama) | 8094 | LLM code review |
| `ollama` | 11434 | downloads `qwen2.5-coder:7b` (~4.7 GB) on first run |
| `redpanda` | 9092 / 8082 / 8081 / 9644 | Kafka API + Schema Registry + Pandaproxy + Admin |
| `questdb` | 9000 / 9009 / 8812 | HTTP + ILP TCP + Postgres wire |
| `redis` | 6379 | — |
| `minio` | 9100 (S3), 9101 (console) | — |
| `registry` | 5000 | Docker registry for built submissions |
| `prometheus` | 9090 | — |
| `grafana` | 3001 | — |
| `jaeger` | 16686 (UI), 4317/4318 (OTLP) | — |

Heads up on the Ollama service: it sits behind `profile: [apps, llm]`
and pulls a ~5 GB model on first start. If you don't need
code-critique, skip it:

```powershell
# Bring up everything except ollama + critique-service:
docker compose --profile default up -d
docker compose --profile apps up -d `
  --scale ollama=0 `
  --scale ollama-init=0 `
  --scale critique-service=0
```

---

## 8. Verify it works

Once `up-apps` is healthy, run the end-to-end smoke test:

### Windows

```powershell
.\scripts\dev-up.ps1 sample-submit
```

### Linux/macOS/WSL

```bash
make sample-submit
```

This runs `scripts/e2e-smoke.sh`, which:

1. Curls `GET /healthz` on the gateway.
2. Tars up `scripts/sample-exchange` (the bundled reference matching
   engine) into a Dockerfile context.
3. `POST /v1/submissions` → uploads the artefact, MinIO stores it.
4. `POST /v1/submissions/{id}/build` → submission-engine triggers
   Kaniko, pushes the image to the local registry.
5. `POST /v1/submissions/{id}/deploy` → submission-engine launches the
   sandbox pod.
6. `POST /v1/benchmarks { profile: baseline }` → bot-controller fans
   out load.
7. Waits 15 s, `POST /v1/benchmarks/{id}/cancel`, sleeps 3 s for
   scoring to flush.
8. `GET /v1/leaderboard?n=50` → asserts at least one row with
   `composite_score > 0`.

You should see green `==>` step markers ending with `PASS`. Then open
**http://localhost:3000** in a browser — the leaderboard row should be
visible and the WebSocket should be connected (top-right pulse
indicator).

---

## 9. Day-to-day commands

All of these go through `docker compose`, so they work identically
across OSes.

| Goal | PowerShell | Make |
|---|---|---|
| Tail all logs | `.\scripts\dev-up.ps1 logs` | `make logs` |
| List running services | `.\scripts\dev-up.ps1 ps` | `make ps` |
| Stop the stack (keep data) | `.\scripts\dev-up.ps1 down` | `make down` |
| Stop + wipe all volumes | `.\scripts\dev-up.ps1 nuke` | `make nuke` |
| Rebuild after code change | `docker compose --profile apps build <service>` | `make build` |
| Regenerate proto stubs | `.\scripts\dev-up.ps1 proto` | `make proto` |
| Run all tests | — | `make test` (C++ + Go + frontend) |
| Frontend hot-reload outside Docker | `cd frontend; npm install; npm run dev` | same |

**Rebuild one service after edits** (example: api-gateway C++ code
changed):

```powershell
docker compose --profile apps build api-gateway
docker compose --profile apps up -d api-gateway
```

---

## 10. Frontend-only development

If you're only touching the React/TypeScript UI, skip Docker for the
frontend and run it natively against the containerised backend:

```powershell
# Backend in containers:
.\scripts\dev-up.ps1 up-apps

# Frontend on the host with hot reload:
cd frontend
npm install
npm run dev
# → http://localhost:3000 (Next.js dev server with HMR)
```

The frontend reads `NEXT_PUBLIC_API_GATEWAY_URL=http://localhost:8080`
and `NEXT_PUBLIC_LEADERBOARD_WS=ws://localhost:8090/v1/leaderboard`
from `.env.local`, which already point at the dockerised gateway and
WS server.

To preview the UI **without any backend** (e.g. on a laptop without
Docker):

```powershell
# In frontend/.env.local
NEXT_PUBLIC_DEMO_MODE=1
npm run dev
```

Every data hook now serves canned data from `frontend/lib/demo-data.ts`.

---

## 11. SDK builds

Three SDKs live under `sdk/`. They are independent of the runtime stack.

```bash
# TypeScript SDK
cd sdk/ts && npm install && npm run build       # output → sdk/ts/dist

# Go SDK
cd sdk/go && go build ./...

# Python SDK (requires Python 3.11+ and `pip install build`)
cd sdk/python && python -m build                 # output → sdk/python/dist
```

Or build all three at once: `make sdk-build`.

---

## 12. Production / Kubernetes deployment

The repo ships a real Helm chart at `infra/helm/velocity` and Terraform
modules at `infra/terraform/{digitalocean,gcp}`.

```bash
# Lint locally without a cluster
make helm-lint

# Render the chart and dry-run apply
make helm-template

# Install into the current kube context (requires kubectl + helm)
RELEASE=demo NAMESPACE=velocity-system make helm-install
```

For multi-tenant production you must also set in `values.yaml`:

- `apiGateway.requireAuth: true`
- `apiGateway.jwtSecretBase64: "<base64url HS256 secret>"`
- `global.registry: "<your registry>"` (default is
  `ghcr.io/velocity-platform`)
- `global.imagePullSecrets: [...]` if your registry is private

Terraform is **optional infrastructure provisioning** for DigitalOcean
or GCP — you'd run it once to create a k3s/GKE cluster, then
`helm install` into it. Most people skip Terraform and bring their own
cluster.

---

## 13. Troubleshooting

Real failures observed against this repo, and the fix for each.

| Symptom | Cause | Fix |
|---|---|---|
| `docker compose pull` 404s on some images | One image moved/yanked | `--ignore-pull-failures` is already set; safe to ignore — compose will build missing apps from source. |
| `bot-worker` exits with `mlock failed` or `io_uring_setup: EPERM` | Docker Desktop's seccomp is too strict | The compose file already sets `security_opt: seccomp:unconfined` and `ulimits.memlock: -1`. If still failing, update Docker Desktop. |
| `submission-engine` can't reach Docker | Compose mounts `/var/run/docker.sock` — Windows/WSL exposes it correctly only if Docker Desktop's *"Expose daemon on tcp://localhost:2375 without TLS"* is **off** and WSL integration is **on** | Reconfigure Docker Desktop as in §3. |
| `ollama-init` stuck on "waiting for ollama…" | First model pull is downloading qwen2.5-coder:7b (~5 GB) | Just wait, or skip with `--scale ollama=0`. |
| Frontend works but leaderboard stays empty | No submission has finished scoring yet | Run `make sample-submit` first. |
| `make proto` fails with permission errors on Windows | `docker run -v "$PWD/proto:/workspace"` doesn't quote-escape on PowerShell | Use the PS1 wrapper (`.\scripts\dev-up.ps1 proto`) — it handles Windows paths. |
| TS-server complains "Cannot find module" for files that exist | Stale TS-server cache (especially on OneDrive paths) | Restart the TS-server in your editor, or move the repo out of OneDrive. |
| `docker compose build` runs out of RAM during C++ link step | <12 GB allocated to Docker | Bump Docker Desktop's memory limit; rebuild. |
| `port 8080 already in use` | Local IIS / another dev service grabbed it | `Get-NetTCPConnection -LocalPort 8080` to find the owner, or edit the port mapping in `docker-compose.yml`. |

---

## 14. One-page TL;DR

For a brand-new laptop, in order:

```powershell
# 1. Install Docker Desktop, Git for Windows. Configure Docker for 12 GB RAM.
# 2. Clone outside OneDrive:
cd C:\dev
git clone <repo-url> velocity
cd velocity\platform

# 3. One-time bootstrap (downloads ~6 GB of base images, generates proto stubs):
.\scripts\bootstrap.ps1

# 4. Frontend env file:
Copy-Item frontend\.env.example frontend\.env.local

# 5. Build every service image (15–30 min first time):
docker compose --profile apps build

# 6. Start everything:
.\scripts\dev-up.ps1 up-apps

# 7. Verify end-to-end:
.\scripts\dev-up.ps1 sample-submit

# 8. Open the UI:
start http://localhost:3000
```

That's the entire path from clean machine to running platform.
