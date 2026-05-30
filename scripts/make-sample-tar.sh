#!/usr/bin/env bash
# =============================================================================
#  make-sample-tar.sh
#
#  Writes the bundled sample matching-engine as a Dockerfile-context tar that
#  you can upload through the frontend's /submissions form (kind = DOCKERFILE).
#  This is the SAME artefact scripts/e2e-smoke.sh builds and uploads, factored
#  out so you can grab the file and upload it from the browser.
#
#  Usage:
#    bash scripts/make-sample-tar.sh [output.tar]
#  Default output: <repo-root>/sample-context.tar
# =============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")"/.. && pwd)"
SAMPLE_DIR="${ROOT_DIR}/scripts/sample-exchange"
OUT="${1:-${ROOT_DIR}/sample-context.tar}"

[ -d "${SAMPLE_DIR}" ] || { echo "missing ${SAMPLE_DIR}" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

cp -r "${SAMPLE_DIR}" "${WORK}/sample-exchange"

# Reuse the binary already compiled inside velocity/cpp-base:builder, then drop
# it into a thin runtime image. Makes the build instant and produces a real,
# correct matching engine — exactly what the bot fleet benchmarks against.
cat > "${WORK}/Dockerfile" <<'DOCKERFILE'
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

tar -C "${WORK}" -cf "${OUT}" .
echo "wrote ${OUT} ($(du -h "${OUT}" | awk '{print $1}'))"
