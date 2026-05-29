# =============================================================================
#  chaos-orchestrator — HTTP control-plane for chaos injection.
# =============================================================================
FROM golang:1.22-bookworm AS builder

WORKDIR /src

# Copy module + sum first for layer caching.
COPY services/chaos-orchestrator/go.mod ./services/chaos-orchestrator/
COPY services/chaos-orchestrator/go.sum ./services/chaos-orchestrator/

WORKDIR /src/services/chaos-orchestrator
RUN go mod download

COPY services/chaos-orchestrator/ ./

RUN CGO_ENABLED=0 GOOS=linux go build \
    -ldflags="-s -w" \
    -o /out/chaos-orchestrator \
    ./cmd/chaos-orchestrator

FROM gcr.io/distroless/static-debian12:nonroot

COPY --from=builder /out/chaos-orchestrator /chaos-orchestrator

EXPOSE 8090
USER nonroot:nonroot
ENTRYPOINT ["/chaos-orchestrator"]
