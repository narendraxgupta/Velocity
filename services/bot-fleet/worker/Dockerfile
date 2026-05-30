# =============================================================================
# velocity/bot-worker
#
# Note the security_opt / ulimit notes in infra/compose/services.yml — the
# worker needs `memlock` raised for io_uring buffer registration. In K8s
# this maps to a SecurityContext with `RLIMIT_MEMLOCK` raised by the kubelet.
# =============================================================================

ARG VELOCITY_BUILDER_IMAGE=velocity/cpp-base:builder
FROM ${VELOCITY_BUILDER_IMAGE} AS builder
RUN cmake --build --preset conan-release --target velocity-bot-worker

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libssl3 libatomic1 liburing2 libatomic1 tini \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid 10001 velocity \
 && useradd  --system --uid 10001 --gid velocity velocity

COPY --from=builder /app/build/Release/bin/velocity-bot-worker \
                    /usr/local/bin/velocity-bot-worker

USER velocity
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/velocity-bot-worker"]
