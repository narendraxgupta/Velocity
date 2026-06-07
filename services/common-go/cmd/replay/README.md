# Offline Replay CLI

This small CLI consumes the event topic and prints events to stdout (JSON).
It is intended as a lightweight harness for exporting and inspecting event
streams for a benchmark run.

Build and run (requires Go toolchain inside the dev container):

```bash
cd services/common-go/cmd/replay
go build -o velocity-replay
./velocity-replay -brokers localhost:9092 -topic events
```

To capture output to a file:

```bash
./velocity-replay -brokers localhost:9092 -topic events > /tmp/benchmark-events.ndjson
```

Notes:

- This is a prototype. The production tool should support partition/offset,
  filtering by `benchmark_id`, and writing ndjson files per benchmark.
- The CLI currently uses the `RedpandaEventStore.Replay` method which
  iterates over the topic and invokes a handler for every event.
