# Velocity TypeScript SDK

> **Preview:** not yet integration-tested against the gateway — verify
> request/response shapes against `services/api-gateway/src/routes/*` and
> `proto/`. See `sdk/README.md` for the full caveat.

```bash
npm install @velocity/sdk
```

```ts
import { VelocityClient } from '@velocity/sdk'

const client = new VelocityClient({
  baseUrl: 'https://demo.velocityhq.io',
  bearer: process.env.VELOCITY_TOKEN,
})

const submission = await client.submissions.create({
  team: 'acme',
  display: 'low-latency-matching-v3',
  kind: 'matching_engine',
  source: tarballBlob,
})

const benchmark = await client.benchmarks.start({
  submissionId: submission.id,
  profile: 'baseline',
})

for await (const event of client.benchmarks.watch(benchmark.id)) {
  console.log(`phase=${event.phase} p99=${event.latencyNs.p99}ns`)
}
```

## Streaming

`watch()` returns an async iterator backed by `EventSource` in the
browser and a streaming `fetch()` in Node 20+. The iterator naturally
terminates when the benchmark completes or is cancelled. Errors
during streaming throw on the next iteration — wrap the `for await`
in a `try / catch`.

## Versioning

Semver, with the major rev-locked to the Velocity API version
(`/v1/...`).
