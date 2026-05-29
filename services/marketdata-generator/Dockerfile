# =============================================================================
#  marketdata-generator — multi-stage build, distroless runtime.
# =============================================================================
ARG GO_VERSION=1.22
FROM golang:${GO_VERSION}-bookworm AS builder
WORKDIR /src
COPY go.mod ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
    go build -trimpath -ldflags "-s -w" \
    -o /out/marketdata-generator ./cmd/marketdata-generator

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=builder /out/marketdata-generator /usr/local/bin/marketdata-generator
USER 65532:65532
EXPOSE 8093
ENTRYPOINT ["/usr/local/bin/marketdata-generator"]
