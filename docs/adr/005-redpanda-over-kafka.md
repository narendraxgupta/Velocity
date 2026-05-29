# ADR-005: Redpanda over Apache Kafka

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: Platform team

## Context

The platform needs a durable event bus to decouple bot publishers from
ingester/validator consumers. The bus must:

1. Sustain ≥1M messages/sec with sub-10ms producer ack latency.
2. Provide partition-keyed ordering for per-submission sequential
   consistency.
3. Support consumer groups so we can horizontally scale the ingester.
4. Run in a single container in dev with a one-line config.
5. Be drop-in replaceable in production with a 3-node cluster.

The de facto industry choice is **Apache Kafka**. We need to explain why we
are not using it directly.

## Decision

We use **Redpanda 24.x**. It speaks the Kafka wire protocol so all our client
libraries (`librdkafka` in C++, `franz-go` in Go, `kafkajs` in TypeScript) work
unchanged.

## Consequences

### Good

- **No JVM.** Single C++ binary; container image ~150 MB vs ~600 MB for
  Confluent Platform.
- **No ZooKeeper / KRaft setup.** Redpanda has its own Raft implementation
  built into the broker process.
- **Lower P99 producer latency** — Redpanda's thread-per-core model with
  `seastar` avoids the JVM's context-switch / GC pauses that show up in
  Kafka's tail latency.
- **Faster cold start** — relevant for our dev loop. `docker compose up`
  has Redpanda accepting connections in ~3 s, vs ~15 s for Kafka + KRaft.
- **Kafka wire compatibility** — every Kafka tool (`rpk`, `kafkactl`,
  `kcat`, the Kafka UI) works against Redpanda. Zero lock-in.

### Bad

- **Smaller ecosystem of managed offerings** than Kafka (Confluent Cloud,
  MSK, etc.). If we move to a managed prod environment, we choose
  Redpanda Cloud or self-host.
- **Some Kafka edge features** are not supported (Kafka Connect plugins
  ecosystem, some transactional patterns). We do not depend on either.
- **Less battle-tested at extreme scale** than Kafka. Our scale is well
  inside Redpanda's proven envelope (1M msg/sec on a single broker is
  Redpanda's headline number; we need ~1M total across 3 brokers).

## Alternatives considered

### Apache Kafka (Confluent Platform / OSS)

- **Pro**: Industry standard. Largest ecosystem. Most mature.
- **Con**: JVM tail-latency variability is well-documented and would
  contaminate our measurement-quality narrative.
- **Con**: Operational footprint (KRaft mode is acceptable, but still more
  pieces than Redpanda).

### NATS JetStream

- **Pro**: Smallest footprint of all options. Single binary. Excellent
  Go-native client.
- **Pro**: Subject-based routing is elegant for our use case.
- **Con**: Persistent storage path for JetStream is still maturing relative
  to Kafka log compaction.
- **Con**: Wire format is NATS-specific; we lose tooling compatibility.
- **Con**: Stream / consumer semantics differ enough from Kafka that we
  would have to rewrite the ingester. We prefer to preserve the
  Kafka-shaped contract so future migrations stay cheap.

### Pulsar

- **Pro**: Two-tier architecture (broker + BookKeeper) scales storage
  independently of compute.
- **Con**: Heavier than Redpanda, no obvious advantage at our scale.
- **Con**: JVM (BookKeeper) — same tail-latency concern as Kafka.

### Redis Streams

- **Pro**: Already in our stack for the leaderboard.
- **Con**: Single-shard write throughput tops out around 100k msg/sec —
  insufficient for our peak.
- **Con**: Consumer group semantics are weaker than Kafka's; replay is
  awkward.

## Migration path

If Redpanda becomes unviable (e.g., a missing Kafka feature surfaces), we
swap broker images. The client libraries (`librdkafka`) are identical. The
topic data does not migrate — but we are in a benchmarking context where
data is ephemeral, so this is a zero-downtime change.

## References

- [Redpanda thread-per-core architecture](https://redpanda.com/blog/tpc-buffers)
- [Benchmarking Redpanda vs Kafka (OpenMessaging)](https://redpanda.com/blog/redpanda-vs-kafka-performance-benchmark)
- [Confluent KRaft mode docs](https://docs.confluent.io/platform/current/installation/configuration/kraft-config.html)
