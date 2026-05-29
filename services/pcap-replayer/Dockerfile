# =============================================================================
#  pcap-replayer — multi-stage build
#
#  No CAP_NET_RAW required (we replay over a regular TCP socket); a tight
#  distroless base is fine. The pcap parsing happens entirely in userspace
#  via google/gopacket — no libpcap shared object on the runtime image.
# =============================================================================
ARG GO_VERSION=1.22
FROM golang:${GO_VERSION}-bookworm AS builder
WORKDIR /src
COPY go.mod ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
    go build -trimpath -ldflags "-s -w" \
    -o /out/pcap-replayer ./cmd/pcap-replayer

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=builder /out/pcap-replayer /usr/local/bin/pcap-replayer
USER 65532:65532
EXPOSE 8092
ENTRYPOINT ["/usr/local/bin/pcap-replayer"]
