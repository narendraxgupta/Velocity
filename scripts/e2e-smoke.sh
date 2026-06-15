#!/usr/bin/env bash
# =============================================================================
#  e2e-smoke.sh
#
#  End-to-end smoke test. Assumes the full docker-compose stack is up
#  (see `make up` / `make up-apps`). The test:
#
#    1. Builds a Dockerfile-context tar for the sample matching engine and
#       uploads it via POST /v1/submissions  (artefact -> MinIO via engine).
#    2. POSTs /v1/submissions/<id>/build      (engine -> Kaniko -> registry).
#    3. POSTs /v1/submissions/<id>/deploy     (engine -> K8s sandbox).
#    3b.Seeks the correctness-validator consumer group to the live edge so it
#       processes THIS run's telemetry instead of crawling a stale backlog
#       (Codespace/shared-CPU only; SMOKE_RESET_VALIDATOR=0 to skip).
#    4. Starts a benchmark and waits 15s. By default (SMOKE_TARGET_RPS=1500)
#       it drives the controller's gRPC override at a single-box-achievable
#       rate; set SMOKE_TARGET_RPS= to POST /v1/benchmarks { profile=baseline }.
#    5. Cancels the benchmark and waits a couple seconds for scoring.
#    6. Polls /v1/leaderboard and asserts a row exists with
#       composite_score > 0.
#
#  Exits 0 on success, non-zero with a diagnostic on failure. Logs every
#  step so the failure mode is obvious in CI.
# =============================================================================
set -euo pipefail

GATEWAY="${VELOCITY_GATEWAY_URL:-http://localhost:8080}"
TEAM="${SMOKE_TEAM:-smoke}"
DISPLAY="${SMOKE_DISPLAY:-smoke-sample-exchange}"
TIMEOUT="${SMOKE_BENCH_SECS:-15}"

step() { printf "\n\033[1;36m==>\033[0m %s\n" "$*" >&2; }
fail() { printf "\n\033[1;31mFAIL:\033[0m %s\n" "$*" >&2; exit 1; }

require() {
  command -v "$1" >/dev/null 2>&1 || fail "missing dependency: $1"
}

require curl
require jq
require tar
require sha256sum

# ----------------------------------------------------------------------------- 0
step "Gateway health"
curl -fsS "${GATEWAY}/healthz" >/dev/null || fail "gateway not healthy at ${GATEWAY}"

# ----------------------------------------------------------------------------- 1
step "Build sample artefact tarball"
ROOT_DIR="$(cd "$(dirname "$0")"/.. && pwd)"
SAMPLE_DIR="${ROOT_DIR}/scripts/sample-exchange"
TARBALL="$(mktemp --suffix=.tar)"

# Build a minimal Dockerfile context: the sample sources + a Dockerfile.
WORK="$(mktemp -d)"
cp -r "${SAMPLE_DIR}" "${WORK}/sample-exchange"
cat > "${WORK}/Dockerfile" <<'DOCKERFILE'
# Smoke-test image for the sample matching engine.
#
# The sample-exchange CMake target links velocity::common and is built as part
# of the monorepo, so it CANNOT be compiled standalone from just this directory.
# Instead we reuse the binary already compiled inside velocity/cpp-base:builder
# (root CMakeLists adds scripts/sample-exchange), then drop it into a thin
# runtime image. This makes the build instant and produces a real, correct
# matching engine — exactly what the bot fleet benchmarks against.
FROM velocity/cpp-base:builder AS build

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 libatomic1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /app/build/Release/bin/velocity-sample-exchange /usr/local/bin/exchange
# The docker sandbox passes EXCHANGE_PORT; default to 8080 to match the
# bot-controller's fabricated endpoint when run standalone.
ENV EXCHANGE_PORT=8080
EXPOSE 8080
CMD ["/usr/local/bin/exchange"]
DOCKERFILE

tar -C "${WORK}" -cf "${TARBALL}" .
echo "  artefact size: $(du -h "${TARBALL}" | awk '{print $1}')"

# ----------------------------------------------------------------------------- 2
step "Upload submission"
SUB_RESP="$(curl -fsS -X POST \
  --data-binary "@${TARBALL}" \
  -H "Content-Type: application/octet-stream" \
  "${GATEWAY}/v1/submissions?team=${TEAM}&display=${DISPLAY}&kind=DOCKERFILE&filename=context.tar")"
SUB_ID="$(echo "${SUB_RESP}" | jq -er '.submission_id')"
echo "  submission_id: ${SUB_ID}"

# ----------------------------------------------------------------------------- 3
step "Build submission"
curl -fsS -X POST "${GATEWAY}/v1/submissions/${SUB_ID}/build" >/dev/null

step "Deploy submission"
curl -fsS -X POST "${GATEWAY}/v1/submissions/${SUB_ID}/deploy" >/dev/null

# ----------------------------------------------------------------------------- 4
# Single-box / Codespace mode. The production `baseline` profile offers 50k rps
# and scores against a 30us p99 — correct for the kernel-bypass cluster this
# platform targets, but unreachable over HTTP on a shared-CPU box. There the
# offered load (50k) dwarfs what the engine can complete (~hundreds/s): requests
# backlog so p99 blows up to seconds, completions trickle back while the
# validator keeps replaying intents, and the resulting missing-fill flood pins
# penalty at 50 -> composite 0 for an otherwise-correct engine.
#
# When SMOKE_TARGET_RPS is set (default 1500) we drive the run at an achievable
# rate via the controller's gRPC `override` (BenchmarkService/StartBenchmark)
# instead of the gateway's named profile. Set SMOKE_TARGET_RPS= (empty) to
# exercise the real 50k baseline through the gateway on a cluster that can take
# it. Pair this with the scoring-service's VELOCITY_DEFAULT_TARGET_RPS /
# VELOCITY_BASELINE_LATENCY_NS (see infra/compose/services.yml).
SMOKE_TARGET_RPS="${SMOKE_TARGET_RPS-1500}"
CONTROLLER_GRPC="${VELOCITY_CONTROLLER_GRPC:-bot-controller:7002}"
CONTROLLER_NET="${VELOCITY_CONTROLLER_NET:-velocity-apps}"
GRPCURL_IMAGE="${GRPCURL_IMAGE:-fullstorydev/grpcurl}"

# Reset the correctness-validator to the live edge of telemetry.raw before the
# run. On a shared-CPU box (e.g. a Codespace) the validator can't drain the
# topic as fast as the bot fleet fills it, so it accrues permanent consumer lag
# (millions of events) across runs. A new submission's telemetry then sits
# behind that backlog and never gets a correctness score within the run window,
# so its leaderboard row never materialises. Seeking the group to the end -- the
# same state a `--force-recreate` leaves it in, but instant and rebuild-free --
# makes the validator process THIS run's telemetry live. Best-effort: a failure
# here must not abort the smoke on a cluster where the validator keeps pace.
SMOKE_RESET_VALIDATOR="${SMOKE_RESET_VALIDATOR:-1}"
VALIDATOR_CONTAINER="${VALIDATOR_CONTAINER:-velocity-correctness-validator}"
REDPANDA_CONTAINER="${REDPANDA_CONTAINER:-velocity-redpanda}"
VALIDATOR_GROUP="${VALIDATOR_GROUP:-velocity-validator}"
TELEMETRY_TOPIC="${TELEMETRY_TOPIC:-telemetry.raw}"

if [ "${SMOKE_RESET_VALIDATOR}" = "1" ] && command -v docker >/dev/null 2>&1; then
  step "Reset validator to live edge (drop backlog so this run scores)"
  if docker ps --format '{{.Names}}' | grep -qx "${VALIDATOR_CONTAINER}"; then
    # The group must be empty for an external offset seek, so stop the lone
    # member first, seek, then bring it back at the live edge.
    docker stop "${VALIDATOR_CONTAINER}" >/dev/null 2>&1 || true
    sleep 2
    docker exec "${REDPANDA_CONTAINER}" rpk group seek "${VALIDATOR_GROUP}" \
      --to end --topics "${TELEMETRY_TOPIC}" >/dev/null 2>&1 \
      || echo "  (group seek best-effort; continuing)" >&2
    docker start "${VALIDATOR_CONTAINER}" >/dev/null 2>&1 || true
    sleep 3
  else
    echo "  (validator container '${VALIDATOR_CONTAINER}' not found; skipping)" >&2
  fi
fi

if [ -n "${SMOKE_TARGET_RPS}" ]; then
  require docker
  step "Start benchmark via controller override (${SMOKE_TARGET_RPS} rps)"
  BENCH_RESP="$(docker run --rm --network "${CONTROLLER_NET}" "${GRPCURL_IMAGE}" -plaintext -d \
    "{\"submission_id\":{\"value\":\"${SUB_ID}\"},\"override\":{\"target_rps\":${SMOKE_TARGET_RPS},\"ramp_seconds\":5,\"hold_seconds\":30}}" \
    "${CONTROLLER_GRPC}" velocity.orchestrator.v1.BenchmarkService/StartBenchmark)"
else
  step "Start baseline benchmark"
  BENCH_RESP="$(curl -fsS -X POST \
    -H "Content-Type: application/json" \
    -d "{\"submission_id\":\"${SUB_ID}\",\"profile\":\"baseline\"}" \
    "${GATEWAY}/v1/benchmarks")"
fi
BENCH_ID="$(echo "${BENCH_RESP}" | jq -er '.benchmark_id')"
echo "  benchmark_id: ${BENCH_ID}"

step "Letting benchmark run for ${TIMEOUT}s"
sleep "${TIMEOUT}"

# ----------------------------------------------------------------------------- 5
step "Cancel benchmark"
# Best-effort: the gateway forwards CancelBenchmark to the same controller that
# owns the run regardless of who started it, but a run started via the gRPC
# override also self-terminates after ramp+hold, so a failed cancel is not fatal.
curl -fsS -X POST "${GATEWAY}/v1/benchmarks/${BENCH_ID}/cancel?reason=smoke" >/dev/null \
  || echo "  (cancel best-effort; run will self-terminate)" >&2
sleep 3   # give the scoring service time to flush a leaderboard delta

# ----------------------------------------------------------------------------- 6
step "Check leaderboard"
LB="$(curl -fsS "${GATEWAY}/v1/leaderboard?n=50")"
echo "${LB}" | jq -C . >&2

# The gateway exposes leaderboard entries under `.entries[]` (see
# services/api-gateway/src/routes/leaderboard.cpp). Keep this matcher in
# sync with the JSON contract.
ROW="$(echo "${LB}" | jq --arg id "${SUB_ID}" \
  '.entries[]? | select(.submission_id == $id)')"
if [ -z "${ROW}" ]; then
  fail "no leaderboard row materialised for ${SUB_ID}"
fi

SCORE="$(echo "${ROW}" | jq -er '.composite_score // 0')"
echo "  composite_score: ${SCORE}"

# jq doesn't do float comparison directly; use awk.
if awk -v s="${SCORE}" 'BEGIN { exit !(s+0 > 0) }'; then
  step "PASS"
  exit 0
else
  fail "composite_score is not > 0 (got ${SCORE})"
fi
