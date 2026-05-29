# =============================================================================
#  critique-service — local-LLM strategy reviewer.
# =============================================================================
ARG GO_VERSION=1.22
FROM golang:${GO_VERSION}-bookworm AS builder
WORKDIR /src
COPY go.mod ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
    go build -trimpath -ldflags "-s -w" \
    -o /out/critique-service ./cmd/critique-service

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=builder /out/critique-service /usr/local/bin/critique-service
USER 65532:65532
EXPOSE 8094
ENTRYPOINT ["/usr/local/bin/critique-service"]
