ARG VELOCITY_BUILDER_IMAGE=velocity/cpp-base:builder
FROM ${VELOCITY_BUILDER_IMAGE} AS builder
RUN cmake --build --preset conan-release --target velocity-correctness-validator

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libssl3 tini \
 && rm -rf /var/lib/apt/lists/* \
 && addgroup --system --gid 10001 velocity \
 && adduser  --system --uid 10001 --ingroup velocity --no-create-home velocity

COPY --from=builder /app/build/Release/bin/velocity-correctness-validator \
                    /usr/local/bin/velocity-correctness-validator

USER velocity
EXPOSE 9096
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/velocity-correctness-validator"]
