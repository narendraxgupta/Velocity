# Velocity Go SDK

A small, hand-written wrapper around Velocity's HTTP + gRPC contracts.
Targets the same `/v1/...` surface a browser would hit, plus the
streaming endpoints (benchmark watch, leaderboard) over Server-Sent
Events.

## Install

```bash
go get github.com/velocity/platform/sdk/go@latest
```

## Quick start

```go
client, err := velocity.NewClient(
    velocity.WithBaseURL("https://demo.velocityhq.io"),
    velocity.WithBearer(os.Getenv("VELOCITY_TOKEN")),
)
if err != nil { log.Fatal(err) }

sub, err := client.Submissions.Create(ctx, &velocity.CreateSubmissionRequest{
    Team:         "acme",
    Display:      "low-latency-matching-v3",
    Kind:         velocity.SubmissionKindMatchingEngine,
    SourceTarGz:  bytes.NewReader(tarball),
})
if err != nil { log.Fatal(err) }

bench, err := client.Benchmarks.Start(ctx, &velocity.StartBenchmarkRequest{
    SubmissionID: sub.ID,
    Profile:      velocity.ProfileBaseline,
})
if err != nil { log.Fatal(err) }

events, err := client.Benchmarks.Watch(ctx, bench.ID)
if err != nil { log.Fatal(err) }
for ev := range events {
    fmt.Printf("phase=%s p99=%dns\n", ev.Phase, ev.LatencyNs.P99)
}
```

## Versioning

Semver. The SDK is rev-locked to a Velocity API version
(`/v1/...`); breaking changes bump the major.

## Generation

The low-level proto/gRPC stubs under `internal/pb` are produced by
`buf generate` (see `proto/buf.gen.yaml`). The wrapper code in `client.go`
and `resources/*.go` is hand-written and lives in this directory.
