import type { VelocityClient } from './client.js'

export type Profile =
  | 'baseline'
  | 'spike'
  | 'fire-hose'
  | 'adversarial'
  | 'cliff-finder'
  | 'cross-venue'

export type LatencyBucket = {
  p50: number
  p90: number
  p99: number
  p999: number
  max: number
}

export type Benchmark = {
  id: string
  submission_id: string
  profile: Profile
  started_at_ns: number
}

export type WatchEvent = {
  benchmark_id: string
  phase: 'warmup' | 'hold' | 'cooldown' | 'complete' | 'cancelled'
  rps: number
  latency_ns: LatencyBucket
  score: number
  ts_ns: number
}

export type StartBenchmarkRequest = {
  submissionId: string
  profile: Profile
}

export class BenchmarksResource {
  constructor(private readonly client: VelocityClient) {}

  async start(req: StartBenchmarkRequest): Promise<Benchmark> {
    const resp = await this.client.request<{ benchmark: Benchmark }>(
      'POST', '/v1/benchmarks',
      { submission_id: req.submissionId, profile: req.profile },
    )
    return resp.benchmark
  }

  async cancel(id: string): Promise<void> {
    await this.client.request<void>(
      'POST', `/v1/benchmarks/${encodeURIComponent(id)}/cancel`,
    )
  }

  watch(id: string, signal?: AbortSignal): AsyncGenerator<WatchEvent, void, void> {
    return this.client.stream<WatchEvent>(
      `/v1/benchmarks/${encodeURIComponent(id)}/watch`, signal,
    )
  }
}
