#!/usr/bin/env bash
# =============================================================================
#  Velocity — Stack Doctor
#
#  One command to surface (almost) every error in a running stack:
#    1. Docker / compose availability
#    2. Container presence, state, health, restart-count
#    3. Recent log scan for error/panic/fatal/exception patterns (per service)
#    4. Host port reachability
#    5. HTTP health/liveness endpoints
#    6. Data-plane probes (Redis PING, Redpanda cluster, QuestDB, MinIO)
#    7. Gateway /v1 smoke checks
#
#  It NEVER mutates state — read-only. Exit code is non-zero if any FAIL.
#
#  Usage:
#    bash scripts/doctor.sh              # full report
#    bash scripts/doctor.sh --logs       # also dump the matched error lines
#    bash scripts/doctor.sh --since 30m  # log window (default 15m)
#    bash scripts/doctor.sh --json        # machine-readable summary only
# =============================================================================

set -uo pipefail

# ----- options ---------------------------------------------------------------
SHOW_LOGS=0
LOG_SINCE="15m"
JSON=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --logs)  SHOW_LOGS=1; shift ;;
    --since) LOG_SINCE="${2:-15m}"; shift 2 ;;
    --json)  JSON=1; shift ;;
    -h|--help)
      sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# ----- pretty print ----------------------------------------------------------
if [[ -t 1 && "$JSON" -eq 0 ]]; then
  B=$'\e[1m'; DIM=$'\e[2m'; R=$'\e[31m'; G=$'\e[32m'; Y=$'\e[33m'; C=$'\e[36m'; X=$'\e[0m'
else
  B=""; DIM=""; R=""; G=""; Y=""; C=""; X=""
fi

PASS=0; FAIL=0; WARN=0
declare -a FAILURES=()

ok()   { printf "  ${G}✓${X} %s\n" "$1"; PASS=$((PASS+1)); }
warn() { printf "  ${Y}!${X} %s\n" "$1"; WARN=$((WARN+1)); }
bad()  { printf "  ${R}✗${X} %s\n" "$1"; FAIL=$((FAIL+1)); FAILURES+=("$1"); }
hdr()  { printf "\n${B}%s${X}\n" "$1"; }

# Docker compose project name (set in docker-compose.yml: `name: velocity`).
PROJECT="velocity"

# Resolve repo root so the script works from anywhere.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# ----- 0. tooling ------------------------------------------------------------
hdr "0 · Tooling"
if command -v docker >/dev/null 2>&1; then
  ok "docker present ($(docker --version | awk '{print $3}' | tr -d ,))"
else
  bad "docker not found in PATH"; printf "\n${R}Cannot continue without docker.${X}\n"; exit 1
fi
if docker compose version >/dev/null 2>&1; then
  ok "docker compose v2 present"
  DC="docker compose"
elif command -v docker-compose >/dev/null 2>&1; then
  warn "using legacy docker-compose v1"
  DC="docker-compose"
else
  bad "docker compose not available"; DC="docker compose"
fi
if ! docker info >/dev/null 2>&1; then
  bad "docker daemon not reachable (is it running?)"
fi

# =============================================================================
#  Service catalogue:  name | host:port for TCP probe | http health url ("" skip)
# =============================================================================
# Container names follow the `velocity-<svc>` convention from the compose file.
SERVICES=(
  "velocity-redpanda|9092|"
  "velocity-questdb|9000|http://localhost:9000"
  "velocity-redis|6379|"
  "velocity-minio|9100|http://localhost:9100/minio/health/live"
  "velocity-registry|5000|http://localhost:5000/v2/"
  "velocity-prometheus|9090|http://localhost:9090/-/healthy"
  "velocity-grafana|3001|http://localhost:3001/api/health"
  "velocity-api-gateway|8080|http://localhost:8080/healthz"
  "velocity-submission-engine|7001|"
  "velocity-bot-controller|7002|"
  "velocity-leaderboard-ws|8090|"
  "velocity-telemetry-ingester||"
  "velocity-correctness-validator||"
  "velocity-scoring-service||"
  "velocity-anomaly-detector|8095|"
  "velocity-marketdata-generator|8093|"
  "velocity-audit-log|8086|"
  "velocity-frontend|3000|http://localhost:3000"
)

# ----- 1. container state ----------------------------------------------------
hdr "1 · Containers (state · health · restarts)"
RUNNING_JSON="$(docker ps -a --format '{{.Names}}' 2>/dev/null || true)"
for entry in "${SERVICES[@]}"; do
  name="${entry%%|*}"
  if ! grep -qx "$name" <<<"$RUNNING_JSON"; then
    warn "$name — not created (profile not up?)"
    continue
  fi
  state="$(docker inspect -f '{{.State.Status}}' "$name" 2>/dev/null || echo '?')"
  health="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}n/a{{end}}' "$name" 2>/dev/null || echo '?')"
  restarts="$(docker inspect -f '{{.RestartCount}}' "$name" 2>/dev/null || echo '?')"
  exitcode="$(docker inspect -f '{{.State.ExitCode}}' "$name" 2>/dev/null || echo '?')"
  label="$name — state=$state health=$health restarts=$restarts"
  if [[ "$state" == "running" && ( "$health" == "healthy" || "$health" == "n/a" ) ]]; then
    if [[ "$restarts" -gt 2 ]] 2>/dev/null; then
      warn "$label (high restart count)"
    else
      ok "$label"
    fi
  elif [[ "$state" == "exited" && "$exitcode" == "0" ]]; then
    ok "$label (one-shot completed)"
  else
    bad "$label exit=$exitcode"
  fi
done

# ----- 2. log error scan -----------------------------------------------------
hdr "2 · Log scan (last $LOG_SINCE)  ·  error|fatal|panic|exception|traceback"
ERR_RE='(^|[^a-zA-Z])(ERROR|FATAL|PANIC|panic:|Exception|Traceback|segfault|terminate called|bind: address already in use|connection refused|no such host|OOMKilled|level=error|"level":"error")'
# Patterns that are noisy/benign — filtered out to cut false positives.
IGNORE_RE='(error_count=0|errors=0|0 errors|errorRate=0|ErrorBoundary|errorElement|"errored":0| errored=0|GET /healthz|/readyz)'
for entry in "${SERVICES[@]}"; do
  name="${entry%%|*}"
  grep -qx "$name" <<<"$RUNNING_JSON" || continue
  logs="$(docker logs --since "$LOG_SINCE" "$name" 2>&1 || true)"
  hits="$(grep -aE "$ERR_RE" <<<"$logs" | grep -avE "$IGNORE_RE" || true)"
  count="$(grep -c . <<<"$hits" 2>/dev/null || echo 0)"
  [[ -z "$hits" ]] && count=0
  if [[ "$count" -eq 0 ]]; then
    ok "$name — clean"
  else
    bad "$name — $count error line(s)"
    if [[ "$SHOW_LOGS" -eq 1 ]]; then
      printf "${DIM}%s${X}\n" "$(tail -n 8 <<<"$hits" | sed 's/^/      /')"
    fi
  fi
done

# ----- 3. host port reachability --------------------------------------------
hdr "3 · Host ports (TCP connect)"
probe_tcp() { # host port
  (exec 3<>"/dev/tcp/$1/$2") 2>/dev/null && { exec 3>&- 3<&-; return 0; } || return 1
}
for entry in "${SERVICES[@]}"; do
  name="${entry%%|*}"; rest="${entry#*|}"; port="${rest%%|*}"
  [[ -z "$port" ]] && continue
  grep -qx "$name" <<<"$RUNNING_JSON" || continue
  if probe_tcp 127.0.0.1 "$port"; then
    ok "$name :$port open"
  else
    bad "$name :$port NOT reachable"
  fi
done

# ----- 4. HTTP health endpoints ---------------------------------------------
hdr "4 · HTTP health endpoints"
http_code() { curl -s -o /dev/null -w '%{http_code}' --max-time 6 "$1" 2>/dev/null || echo 000; }
for entry in "${SERVICES[@]}"; do
  name="${entry%%|*}"; url="${entry##*|}"
  [[ -z "$url" || "$url" == "$name" ]] && continue
  grep -qx "$name" <<<"$RUNNING_JSON" || continue
  code="$(http_code "$url")"
  if [[ "$code" =~ ^(200|204|301|302|401|403)$ ]]; then
    ok "$name $url → $code"
  else
    bad "$name $url → $code"
  fi
done

# ----- 5. gateway /v1 smoke --------------------------------------------------
hdr "5 · Gateway /v1 smoke checks"
if grep -qx "velocity-api-gateway" <<<"$RUNNING_JSON"; then
  for path in "/readyz" "/v1/leaderboard?n=1" "/v1/leaderboard/health" "/v1/fleet" "/v1/marketdata/snapshot"; do
    code="$(http_code "http://localhost:8080${path}")"
    if [[ "$code" =~ ^(200|204)$ ]]; then
      ok "GET ${path} → $code"
    elif [[ "$code" =~ ^(401|403)$ ]]; then
      warn "GET ${path} → $code (auth required)"
    else
      bad "GET ${path} → $code"
    fi
  done
else
  warn "api-gateway not running — skipping /v1 smoke"
fi

# ----- 6. data-plane probes --------------------------------------------------
hdr "6 · Data-plane probes"
# Redis PING
if grep -qx "velocity-redis" <<<"$RUNNING_JSON"; then
  if [[ "$(docker exec velocity-redis redis-cli ping 2>/dev/null)" == "PONG" ]]; then
    zcard="$(docker exec velocity-redis redis-cli ZCARD leaderboard:composite 2>/dev/null || echo '?')"
    ok "redis PING ok · leaderboard:composite entries=$zcard"
  else
    bad "redis PING failed"
  fi
fi
# Redpanda cluster
if grep -qx "velocity-redpanda" <<<"$RUNNING_JSON"; then
  if docker exec velocity-redpanda rpk cluster info >/dev/null 2>&1; then
    topics="$(docker exec velocity-redpanda rpk topic list 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')"
    ok "redpanda cluster healthy · topics=$topics"
  else
    bad "redpanda cluster info failed"
  fi
fi
# QuestDB query
if grep -qx "velocity-questdb" <<<"$RUNNING_JSON"; then
  q="$(curl -fsS --max-time 6 "http://localhost:9000/exec?query=SELECT%201" 2>/dev/null || true)"
  if grep -q '"dataset"' <<<"$q"; then
    ok "questdb query ok"
  else
    bad "questdb query failed"
  fi
fi

# ----- 7. compose config sanity ---------------------------------------------
hdr "7 · Compose config validation"
if $DC --profile default --profile apps config -q >/dev/null 2>&1; then
  ok "compose config parses (default+apps)"
else
  bad "compose config invalid — run: $DC --profile default --profile apps config"
fi

# ----- summary ---------------------------------------------------------------
TOTAL=$((PASS+WARN+FAIL))
if [[ "$JSON" -eq 1 ]]; then
  printf '{"pass":%d,"warn":%d,"fail":%d,"total":%d}\n' "$PASS" "$WARN" "$FAIL" "$TOTAL"
else
  hdr "Summary"
  printf "  ${G}%d passed${X} · ${Y}%d warnings${X} · ${R}%d failed${X}  (of %d checks)\n" "$PASS" "$WARN" "$FAIL" "$TOTAL"
  if [[ "$FAIL" -gt 0 ]]; then
    printf "\n${R}${B}Failures:${X}\n"
    for f in "${FAILURES[@]}"; do printf "  ${R}•${X} %s\n" "$f"; done
    printf "\n${DIM}Tip: re-run with --logs to see the offending log lines, or:\n  %s logs -f <service>${X}\n" "$DC"
  fi
fi

[[ "$FAIL" -eq 0 ]]
