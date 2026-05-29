/**
 * Demo / canned-data mode.
 *
 * When `NEXT_PUBLIC_DEMO_MODE=1` is set at build time, every hook in the
 * frontend skips its network calls and serves data from this module
 * instead. Useful when iterating on the UI without the C++/Go backend
 * running locally (e.g. on a laptop without Docker).
 *
 * The mock data is deliberately *realistic*: composite scores in the 60–90
 * range, latency tails that wiggle, occasional rank flips between two teams
 * jostling for the top spot.
 */

export type DemoLeaderRow = {
  rank: number
  team: string
  submissionId: string
  composite: number
  throughputRps: number
  p99Ns: number
  correctness: number
  status: 'running' | 'scored' | 'queued' | 'dq'
  delta: number
}

export type DemoSubmission = {
  id: string
  team: string
  displayName: string
  status: 'running' | 'scored' | 'queued' | 'dq'
  createdAt: number
  artefactSize: number
  language: string
  benchmarkId?: string
}

export function isDemoMode(): boolean {
  if (typeof process !== 'undefined' && process.env.NEXT_PUBLIC_DEMO_MODE === '1') {
    return true
  }
  if (typeof window !== 'undefined') {
    return window.localStorage?.getItem('velocity:demo') === '1'
  }
  return false
}

const TEAMS: Array<Omit<DemoLeaderRow, 'rank' | 'delta'>> = [
  {
    team: 'Aegis Trading',
    submissionId: '01HQEAGIS001VPK7T7Q1QXKQ8N',
    composite: 87.34,
    throughputRps: 480_300,
    p99Ns: 41_200,
    correctness: 99.94,
    status: 'running',
  },
  {
    team: 'NanoBook',
    submissionId: '01HQNANOBOOK02DFN3YQ8QXAJP',
    composite: 84.91,
    throughputRps: 462_100,
    p99Ns: 44_800,
    correctness: 99.81,
    status: 'running',
  },
  {
    team: 'KernelOps',
    submissionId: '01HQKERNELOPS03MZHRTW9CXJ5',
    composite: 79.22,
    throughputRps: 410_700,
    p99Ns: 56_800,
    correctness: 99.65,
    status: 'scored',
  },
  {
    team: 'TickHaus',
    submissionId: '01HQTICKHAUS04QRYZHJYZ8WJM',
    composite: 71.86,
    throughputRps: 358_900,
    p99Ns: 71_400,
    correctness: 98.92,
    status: 'scored',
  },
  {
    team: 'Tail Latency Hunters',
    submissionId: '01HQTAILLATH05VWHRTW2QY1MP',
    composite: 68.50,
    throughputRps: 340_100,
    p99Ns: 84_700,
    correctness: 98.41,
    status: 'scored',
  },
  {
    team: 'Outpost',
    submissionId: '01HQOUTPOST06KNYXRC8WQJ4ZG',
    composite: 62.39,
    throughputRps: 295_300,
    p99Ns: 106_200,
    correctness: 96.18,
    status: 'scored',
  },
  {
    team: 'Stack & Trade',
    submissionId: '01HQSTACKTRAD07RZWXJKQH9MPP',
    composite: 53.78,
    throughputRps: 251_800,
    p99Ns: 141_400,
    correctness: 94.61,
    status: 'scored',
  },
  {
    team: 'Halt & Catch Fire',
    submissionId: '01HQHALTCATCHFR08QXMVK1H4WP',
    composite: 12.40,
    throughputRps: 25_400,
    p99Ns: 1_240_000,
    correctness: 64.30,
    status: 'dq',
  },
]

export function mockLeaderboard(): DemoLeaderRow[] {
  return TEAMS.map((t, i) => ({
    ...t,
    rank: i + 1,
    delta: i === 0 ? 0 : Math.floor((Math.random() - 0.5) * 3),
  }))
}

export function mockSubmissions(): DemoSubmission[] {
  // Indexed reads return `T | undefined` under noUncheckedIndexedAccess,
  // even when the index is provably bounded (split() always yields at
  // least one element for a string; `i % 5` is always 0..4). Use `??`
  // fallbacks to satisfy the narrower type without changing behaviour.
  const LANGS = ['C++', 'Rust', 'C++', 'Go', 'Zig'] as const
  return TEAMS.slice(0, 5).map((t, i) => ({
    id: t.submissionId,
    team: t.team,
    displayName: `${(t.team.split(/\s+/)[0] ?? t.team).toLowerCase()}-matcher`,
    status: t.status,
    createdAt: Date.now() - (i + 1) * 6 * 3600 * 1000,
    artefactSize: 8_400_000 + i * 1_300_000,
    language: LANGS[i % LANGS.length] ?? 'C++',
    benchmarkId: `BM-DEMO-${(i + 1).toString(16)}`,
  }))
}

/**
 * Returns ~240 snapshots covering the full benchmark lifecycle: 5s queue,
 * 5s ramp, 30s hold, 5s drain. The numbers wiggle plausibly under a tail.
 */
export function mockBenchmarkSnapshots(benchmarkId: string) {
  const phases = (i: number): 'queued' | 'ramping' | 'holding' | 'draining' | 'complete' => {
    if (i < 5) return 'queued'
    if (i < 25) return 'ramping'
    if (i < 145) return 'holding'
    if (i < 155) return 'draining'
    return 'complete'
  }
  // `TARGET` is intentionally widened to `number` so the `=== 0` guard
  // below survives narrowing. Without the annotation TS would infer the
  // literal type `500_000` and flag the guard as a dead comparison —
  // even though the guard exists precisely to make the helper robust
  // against future overrides that set TARGET to 0.
  const TARGET: number = 500_000
  return Array.from({ length: 165 }, (_, i) => {
    const phase = phases(i)
    const t = i / 165
    const rampedRps =
      phase === 'queued' ? 0 :
      phase === 'ramping' ? Math.floor((i - 5) * (TARGET / 20)) :
      phase === 'holding' ? TARGET - Math.floor(Math.sin(i / 4) * 6_000) :
      phase === 'draining' ? Math.floor(TARGET * (1 - (i - 145) / 10)) :
      0
    const p99 = 38_000 + Math.floor(Math.sin(i / 3.7) * 6_500) + Math.floor(t * 4_000)
    const correctness = 99.6 - Math.abs(Math.sin(i / 5)) * 0.4
    const throughput = TARGET === 0 ? 0 : 100 * Math.min(1, Math.max(0, rampedRps) / TARGET)
    const p99_us = p99 / 1000
    const baseline_us = 30
    const latency = p99_us <= baseline_us
      ? 100
      : Math.max(0, 100 - 100 * (p99_us - baseline_us) / baseline_us)
    const penalty = 0
    const composite = Math.max(0, 0.40 * throughput + 0.35 * latency + 0.25 * correctness - penalty)
    return {
      benchmarkId,
      tsMs: 0,    // filled by consumer
      elapsedMs: i * 250,
      phase,
      sentTotal: Math.max(0, i * 4_500),
      ackedTotal: Math.max(0, i * 4_450),
      erroredTotal: Math.max(0, Math.floor(i * 1.2)),
      currentRps: Math.max(0, rampedRps),
      targetRps: TARGET,
      p50Ns: Math.floor(p99 * 0.35),
      p90Ns: Math.floor(p99 * 0.65),
      p99Ns: p99,
      p999Ns: Math.floor(p99 * 1.9),
      maxNs: Math.floor(p99 * 5.2),
      // Kernel-vs-userspace delta — surfaced by the eBPF probe in real
      // runs. For demo we synthesise a stable ~30% kernel share so the
      // skew chart has something to draw. Set kernelSamples=0 to make
      // it explicit when the demo data path is the source.
      kernelP50Ns:   Math.floor(p99 * 0.10),
      kernelP99Ns:   Math.floor(p99 * 0.30),
      kernelP999Ns:  Math.floor(p99 * 0.55),
      kernelSamples: Math.max(0, i * 4_450),
      correctnessScore: correctness,
      compositeScore: composite,
      throughputScore: throughput,
      latencyScore: latency,
      penaltyScore: penalty,
      traceId: 'demo00000000000000000000000000ce',
    }
  })
}
