# Running Velocity in GitHub Codespaces

This `.devcontainer/` makes the whole platform runnable in a cloud Linux box
with Docker, which is what the C++ data-plane needs (`io_uring`, `liburing`,
`pthread_setaffinity_np`, `CLOCK_MONOTONIC_RAW`) — none of which exist on
Windows/macOS natively. See `docs/SETUP.md` for the full reference.

## What this devcontainer sets up

- **Ubuntu 24.04** base with **Docker-in-Docker** (runs the entire
  `docker compose` stack inside the codespace).
- **Node 20, Go 1.22, Python 3.11** for native frontend / Go / Python work.
- Installs `make`, `jq`, `curl` (needed by the Makefile and
  `scripts/e2e-smoke.sh`).
- On create: copies `frontend/.env.example` → `frontend/.env.local` and runs
  `make bootstrap` (pulls base images, generates protobuf stubs).
- Forwards every UI port (frontend, gateway, WS, QuestDB, MinIO, Grafana,
  Prometheus, Redpanda Console, Jaeger).

## Machine size — read this first

The default 2-core / 8 GB codespace **will OOM** during the C++ link step.
When creating the codespace use **Code ▸ Codespaces ▸ "…" ▸ New with
options…** and pick:

- **4-core / 16 GB minimum**
- **8-core / 32 GB recommended** (C++ links several large binaries)

`hostRequirements` in `devcontainer.json` requests 4-core/16 GB as a floor.

> GitHub Student Pack → GitHub Pro → **180 Codespaces core-hours + 20 GB
> storage free / month**. **Stop the codespace when idle** (Codespaces menu ▸
> *Stop Current Codespace*) so you don't burn hours.

## Quick start (inside the codespace terminal)

```bash
# 1. (auto-run on create) one-time bootstrap — re-run if it didn't finish:
make bootstrap

# 2. build all service images (15–30 min cold):
make build
# (equivalently: docker compose --profile apps build)

# 3. start the stack WITHOUT Ollama (saves a ~5 GB model download + RAM):
docker compose --profile default up -d
docker compose --profile apps up -d \
  --scale ollama=0 --scale ollama-init=0 --scale critique-service=0

# 4. prove the full pipeline end-to-end (upload → build → deploy → score):
make sample-submit
```

If you DO want the LLM code-critique service, just run `make up-apps` instead
of step 3 (it pulls `qwen2.5-coder:7b`, ~5 GB).

## Viewing the UI — important gotcha

The frontend uses **client-side `localhost` URLs**
(`NEXT_PUBLIC_API_GATEWAY_URL=http://localhost:8080`,
`NEXT_PUBLIC_LEADERBOARD_WS=ws://localhost:8090/...`). That changes how you
open it:

- **Recommended — open the codespace in VS Code Desktop**
  (*Codespaces menu ▸ Open in VS Code Desktop*). Desktop forwards ports to
  your laptop's real `localhost`, so `localhost:3000`, `:8080`, `:8090` all
  work and the demo "just works." Browse to <http://localhost:3000>.
- **Browser-only Codespaces:** forwarded ports become
  `https://<codespace>-<port>.app.github.dev`, **not** `localhost`. The
  frontend's hardcoded `localhost:8080`/`8090` then won't reach the backend.
  Fix: set `frontend/.env.local`'s `NEXT_PUBLIC_API_GATEWAY_URL` and
  `NEXT_PUBLIC_LEADERBOARD_WS` to the forwarded `https://…-8080…` /
  `wss://…-8090…` URLs, mark ports **8080** and **8090** as **Public** in the
  Ports tab, then rebuild the frontend (`docker compose --profile apps up -d --build frontend`).

## Other consoles (Ports tab)

| Port | Service |
|---|---|
| 3000 | Frontend leaderboard UI |
| 8080 | API Gateway (`/healthz`) |
| 8090 | Leaderboard WebSocket |
| 9000 | QuestDB console |
| 9101 | MinIO console (`velocity` / `velocity-dev-secret`) |
| 3001 | Grafana (`velocity` / `velocity-dev`) |
| 9090 | Prometheus |
| 8085 | Redpanda Console |
| 16686 | Jaeger tracing UI |

## Troubleshooting (Codespaces-specific)

| Symptom | Fix |
|---|---|
| `bot-worker` exits `io_uring_setup: EPERM` / `mlock failed` | The compose file already sets `seccomp:unconfined` + `memlock:-1`. If Docker-in-Docker still sandboxes io_uring, demo with the REST-transport persona. |
| OOM during C++ link | Use an 8-core/32 GB machine, or build services one group at a time: `make build-api`, `make build-bot`, `make build-data`. |
| `no space left on device` | `docker system prune -f`; recreate the codespace with `"storage": "64gb"` in `devcontainer.json`. |
| Leaderboard empty in the UI | Run `make sample-submit` first. |
| Frontend loads but can't reach the API in a browser tab | Use VS Code Desktop, or apply the "Browser-only" fix above. |

> Note: the strict **gVisor** sandbox isolation is a Kubernetes-only
> RuntimeClass feature and does **not** run in the compose demo (the sandbox
> there is launched via `docker.sock`). The Codespaces demo proves the full
> pipeline; to demonstrate gVisor specifically, use the Helm/K8s path in
> `docs/SETUP.md` §12 on a real cluster.
