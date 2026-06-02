# =============================================================================
# velocity/api-gateway
#
# Two-line service Dockerfile pattern: build against the shared cpp-base
# image, then carry one binary into a thin runtime layer.
# =============================================================================

# ---- builder ----------------------------------------------------------------
ARG VELOCITY_BUILDER_IMAGE=velocity/cpp-base:builder
FROM ${VELOCITY_BUILDER_IMAGE} AS builder

COPY services/api-gateway/ ./services/api-gateway/

# Build only this target — faster incremental rebuilds.
RUN cmake --build --preset conan-release --target velocity-api-gateway

# ---- runtime ----------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      libssl3 libatomic1 \
      libatomic1 \
      tini \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid 10001 velocity \
 && useradd  --system --uid 10001 --gid velocity velocity

COPY --from=builder /app/build/Release/bin/velocity-api-gateway \
                    /usr/local/bin/velocity-api-gateway

USER velocity
EXPOSE 8080 9091

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/velocity-api-gateway"]
