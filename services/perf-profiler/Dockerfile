# =============================================================================
#  perf-profiler — sidecar that records a CPU flamegraph for a submission pod.
#
#  Lifecycle (driven by `entrypoint.sh`):
#    1. Wait for the submission container's PID to appear in the shared PID
#       namespace (shareProcessNamespace=true).
#    2. `perf record -F $VELOCITY_PROFILE_SAMPLE_HZ -g -p <pid>` for the
#       full lifetime of the pod (sidecar receives SIGTERM when the pod
#       is torn down).
#    3. Fold the perf data with `inferno-collapse-perf`, save the folded
#       text + an SVG flamegraph.
#    4. Upload both objects to MinIO at
#         flamegraphs/<submission_id>.folded.txt
#         flamegraphs/<submission_id>.svg
#       plus a sibling meta JSON with sample-rate / duration so the
#       gateway can render context on the profile page.
#
#  Image size budget: ~120 MiB; inferno binaries are small Rust statics.
# =============================================================================
FROM rust:1.79-bookworm AS inferno

# Build inferno statically so we don't drag glibc into the runtime image.
RUN cargo install inferno --root /opt/inferno --locked

FROM debian:bookworm-slim AS runtime

ARG TARGETARCH

# linux-perf is the smallest perf binary on Debian; minio-client is the
# simplest way to push to MinIO without bundling AWS SDKs.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        linux-perf \
        procps \
        ca-certificates \
        wget \
    && \
    wget -q "https://dl.min.io/client/mc/release/linux-${TARGETARCH:-amd64}/mc" \
        -O /usr/local/bin/mc && \
    chmod +x /usr/local/bin/mc && \
    apt-get purge -y wget && \
    apt-get autoremove -y && \
    rm -rf /var/lib/apt/lists/*

COPY --from=inferno /opt/inferno/bin/inferno-collapse-perf /usr/local/bin/
COPY --from=inferno /opt/inferno/bin/inferno-flamegraph    /usr/local/bin/

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# Image needs CAP_PERFMON + CAP_SYS_PTRACE granted by the pod spec; see
# services/submission-engine/internal/sandbox/sandbox.go.
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
