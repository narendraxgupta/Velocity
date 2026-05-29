# =============================================================================
# velocity/submission-engine — Go service
#
# Multi-stage:
#   builder  - compile a static binary
#   runtime  - distroless final image
#
# Build context MUST be the platform monorepo root because the Go module
# `replace`s github.com/velocity/platform/proto/gen/go against ../../proto/gen/go
# (see services/submission-engine/go.mod). The compose file already passes
# `context: ..` for this reason.
# =============================================================================

# ---- builder ----------------------------------------------------------------
FROM golang:1.22-alpine AS builder
RUN apk add --no-cache git ca-certificates

WORKDIR /src

# Pull just the manifests first so layer caching is friendly to source-only
# edits. We need both the service module AND the proto gen module that it
# replaces against.
COPY services/submission-engine/go.mod   services/submission-engine/go.sum* services/submission-engine/
COPY proto/gen/go/go.mod                 proto/gen/go/

WORKDIR /src/services/submission-engine
RUN go mod download

# Now copy the rest of what the service compiles, including the generated
# proto bindings. `proto/gen/go` is populated by `make proto` (buf generate).
WORKDIR /src
COPY proto/                              proto/
COPY services/submission-engine/         services/submission-engine/

WORKDIR /src/services/submission-engine
RUN CGO_ENABLED=0 GOOS=linux \
    go build \
        -trimpath \
        -ldflags='-s -w -extldflags=-static' \
        -o /out/velocity-submission-engine \
        ./cmd/submission-engine

# ---- runtime ----------------------------------------------------------------
FROM gcr.io/distroless/static-debian12:nonroot AS runtime

COPY --from=builder /out/velocity-submission-engine /usr/local/bin/velocity-submission-engine

USER nonroot
EXPOSE 7001 9092
ENTRYPOINT ["/usr/local/bin/velocity-submission-engine"]
