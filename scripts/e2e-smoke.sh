#!/usr/bin/env bash
# =============================================================================
#  e2e-smoke.sh
#
#  End-to-end smoke test. Assumes the full docker-compose stack is up
#  (see `make compose-up`). The test:
#
#    1. Builds a Dockerfile-context tar for the sample matching engine and
#       uploads it via POST /v1/submissions  (artefact -> MinIO via engine).
#    2. POSTs /v1/submissions/<id>/build      (engine -> Kaniko -> registry).
#    3. POSTs /v1/submissions/<id>/deploy     (engine -> K8s sandbox).
#    4. POSTs /v1/benchmarks { profile=baseline } and waits 15s.
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
# Smoke-test image: reuse the sample matching engine prebuilt in cpp-base.
FROM velocity/cpp-base:builder AS build

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 libatomic1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /app/build/Release/bin/velocity-sample-exchange /usr/local/bin/exchange
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
step "Start baseline benchmark"
BENCH_RESP="$(curl -fsS -X POST \
  -H "Content-Type: application/json" \
  -d "{\"submission_id\":\"${SUB_ID}\",\"profile\":\"baseline\"}" \
  "${GATEWAY}/v1/benchmarks")"
BENCH_ID="$(echo "${BENCH_RESP}" | jq -er '.benchmark_id')"
echo "  benchmark_id: ${BENCH_ID}"

step "Letting benchmark run for ${TIMEOUT}s"
sleep "${TIMEOUT}"

# ----------------------------------------------------------------------------- 5
step "Cancel benchmark"
curl -fsS -X POST "${GATEWAY}/v1/benchmarks/${BENCH_ID}/cancel?reason=smoke" >/dev/null
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
