#!/usr/bin/env bash
# =============================================================================
#  perf-profiler entrypoint
#
#  Required env vars (set by sandbox.go when wiring the sidecar):
#    VELOCITY_SUBMISSION_ID     — used as the MinIO object key prefix
#    VELOCITY_MINIO_ENDPOINT    — e.g. "minio.velocity-data:9000"
#    VELOCITY_MINIO_ACCESS      — MinIO access key
#    VELOCITY_MINIO_SECRET      — MinIO secret key
#    VELOCITY_MINIO_BUCKET      — defaults to "velocity-artefacts"
#    VELOCITY_PROFILE_SAMPLE_HZ — perf record -F frequency (default 99)
# =============================================================================
set -euo pipefail

: "${VELOCITY_SUBMISSION_ID:?required}"
: "${VELOCITY_MINIO_ENDPOINT:?required}"
: "${VELOCITY_MINIO_ACCESS:?required}"
: "${VELOCITY_MINIO_SECRET:?required}"
: "${VELOCITY_MINIO_BUCKET:=velocity-artefacts}"
: "${VELOCITY_PROFILE_SAMPLE_HZ:=99}"

WORK_DIR=/var/velocity/profile
mkdir -p "${WORK_DIR}"
PERF_DATA="${WORK_DIR}/perf.data"
FOLDED="${WORK_DIR}/${VELOCITY_SUBMISSION_ID}.folded.txt"
SVG="${WORK_DIR}/${VELOCITY_SUBMISSION_ID}.svg"
META="${WORK_DIR}/${VELOCITY_SUBMISSION_ID}.meta.json"

# Find the submission's PID in the shared PID namespace. We assume the
# main process is whichever PID is NOT us and NOT a kernel/runtime pid.
# In practice the submission has PID 1 because we set shareProcessNamespace
# on the pod and the submission container is the first one launched.
wait_for_submission_pid() {
    local i tgt
    for i in $(seq 1 30); do
        # Look for any pid other than self, init, and kthreadd.
        tgt=$(pgrep -v -P 0 | grep -vE "^(1|2|$$)\$" | head -n 1 || true)
        if [[ -n "${tgt}" ]]; then
            echo "${tgt}"
            return 0
        fi
        sleep 1
    done
    return 1
}

SUB_PID="$(wait_for_submission_pid || true)"
if [[ -z "${SUB_PID}" ]]; then
    echo "perf-profiler: could not locate submission pid; exiting" >&2
    exit 0
fi
echo "perf-profiler: attaching to pid=${SUB_PID} hz=${VELOCITY_PROFILE_SAMPLE_HZ}"

# Start perf in the background so we can react to SIGTERM cleanly.
START_NS=$(date +%s%N)
perf record \
    -F "${VELOCITY_PROFILE_SAMPLE_HZ}" \
    -g \
    -p "${SUB_PID}" \
    -o "${PERF_DATA}" \
    &
PERF_PID=$!

# On SIGTERM (pod teardown), stop perf gracefully so the recording is
# finalised; then fold and upload.
finalise() {
    echo "perf-profiler: SIGTERM received; finalising recording"
    kill -INT "${PERF_PID}" || true
    wait "${PERF_PID}" || true
    END_NS=$(date +%s%N)

    if [[ ! -s "${PERF_DATA}" ]]; then
        echo "perf-profiler: no perf.data captured; nothing to upload"
        exit 0
    fi

    # Fold + render.
    perf script -i "${PERF_DATA}" | inferno-collapse-perf > "${FOLDED}"
    inferno-flamegraph < "${FOLDED}" > "${SVG}"

    DURATION_NS=$(( END_NS - START_NS ))
    cat > "${META}" <<EOF
{
  "submission_id": "${VELOCITY_SUBMISSION_ID}",
  "recorded_at_ns": ${START_NS},
  "sample_freq_hz": ${VELOCITY_PROFILE_SAMPLE_HZ},
  "duration_seconds": $(( DURATION_NS / 1000000000 ))
}
EOF

    # Upload all three to MinIO.
    mc alias set velocity \
        "http://${VELOCITY_MINIO_ENDPOINT}" \
        "${VELOCITY_MINIO_ACCESS}" \
        "${VELOCITY_MINIO_SECRET}" >/dev/null

    mc cp --quiet "${FOLDED}" "velocity/${VELOCITY_MINIO_BUCKET}/flamegraphs/${VELOCITY_SUBMISSION_ID}.folded.txt"
    mc cp --quiet "${SVG}"    "velocity/${VELOCITY_MINIO_BUCKET}/flamegraphs/${VELOCITY_SUBMISSION_ID}.svg"
    mc cp --quiet "${META}"   "velocity/${VELOCITY_MINIO_BUCKET}/flamegraphs/${VELOCITY_SUBMISSION_ID}.meta.json"

    echo "perf-profiler: uploaded folded+svg+meta for submission=${VELOCITY_SUBMISSION_ID}"
    exit 0
}
trap finalise SIGTERM SIGINT

# Idle while perf records. We don't `wait $PERF_PID` directly because that
# blocks signal handling on some shells; the busy-loop is cheap and lets
# `trap` fire promptly.
while kill -0 "${PERF_PID}" 2>/dev/null; do
    sleep 5
done
