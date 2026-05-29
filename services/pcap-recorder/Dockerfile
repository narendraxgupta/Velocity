# =============================================================================
#  pcap-recorder — multi-stage build
#
#  Stage 1: build the Go binary in a scratch-on-distroless build env.
#  Stage 2: copy into a minimal nonroot distroless. We do NOT need tcpdump
#  in this image — tcpdump runs in an ephemeral container injected into the
#  *target* pod, where CAP_NET_RAW is granted. The recorder itself only
#  needs the k8s + minio SDKs at runtime.
# =============================================================================
ARG GO_VERSION=1.22
FROM golang:${GO_VERSION}-bookworm AS builder
WORKDIR /src
COPY go.mod ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
    go build -trimpath -ldflags "-s -w" \
    -o /out/pcap-recorder ./cmd/pcap-recorder

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=builder /out/pcap-recorder /usr/local/bin/pcap-recorder
USER 65532:65532
EXPOSE 8091
ENTRYPOINT ["/usr/local/bin/pcap-recorder"]
