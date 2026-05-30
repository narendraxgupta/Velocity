#!/usr/bin/env bash
# =============================================================================
#  reset-validator.sh
#
#  Seek the correctness-validator's consumer group to the live edge of
#  telemetry.raw. On a shared-CPU box (e.g. a GitHub Codespace) the validator
#  cannot drain telemetry as fast as the bot fleet produces it, so it accrues
#  permanent consumer lag (millions of events). A fresh submission's telemetry
#  then sits behind that backlog and never gets a correctness score within the
#  run window -> its leaderboard row never materialises.
#
#  Seeking the group to the end (the same state a `docker compose ...
#  --force-recreate` leaves it in, but instant and rebuild-free) makes the
#  validator process the NEXT run's telemetry live. Run this before starting a
#  benchmark from the UI; the e2e-smoke.sh script already does it automatically.
#
#  All knobs are env-overridable for non-default deployments.
# =============================================================================
set -euo pipefail

VALIDATOR_CONTAINER="${VALIDATOR_CONTAINER:-velocity-correctness-validator}"
REDPANDA_CONTAINER="${REDPANDA_CONTAINER:-velocity-redpanda}"
VALIDATOR_GROUP="${VALIDATOR_GROUP:-velocity-validator}"
TELEMETRY_TOPIC="${TELEMETRY_TOPIC:-telemetry.raw}"

command -v docker >/dev/null 2>&1 || { echo "docker not found; nothing to do" >&2; exit 0; }

if ! docker ps --format '{{.Names}}' | grep -qx "${VALIDATOR_CONTAINER}"; then
  echo "validator container '${VALIDATOR_CONTAINER}' not running; skipping" >&2
  exit 0
fi

echo "resetting ${VALIDATOR_GROUP} to the live edge of ${TELEMETRY_TOPIC} ..."

# The group must be empty for an external offset seek, so stop the lone member
# first, seek, then bring it back at the live edge.
docker stop "${VALIDATOR_CONTAINER}" >/dev/null 2>&1 || true
sleep 2
docker exec "${REDPANDA_CONTAINER}" rpk group seek "${VALIDATOR_GROUP}" \
  --to end --topics "${TELEMETRY_TOPIC}" || echo "  (group seek best-effort)" >&2
docker start "${VALIDATOR_CONTAINER}" >/dev/null 2>&1 || true
sleep 3

docker exec "${REDPANDA_CONTAINER}" rpk group describe "${VALIDATOR_GROUP}" 2>&1 | sed -n '1,12p'