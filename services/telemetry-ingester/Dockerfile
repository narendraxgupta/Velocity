ARG VELOCITY_BUILDER_IMAGE=velocity/cpp-base:builder
FROM ${VELOCITY_BUILDER_IMAGE} AS builder
RUN cmake --build --preset conan-release --target velocity-telemetry-ingester

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libssl3 libatomic1 tini \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid 10001 velocity \
 && useradd  --system --uid 10001 --gid velocity velocity

COPY --from=builder /app/build/Release/bin/velocity-telemetry-ingester \
                    /usr/local/bin/velocity-telemetry-ingester

USER velocity
EXPOSE 9095
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/velocity-telemetry-ingester"]
