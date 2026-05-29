/**
 * useBenchmarkStream — subscribes to the gateway's per-benchmark SSE stream.
 *
 * The gateway endpoint `/v1/benchmarks/{id}/stream` emits JSON payloads as
 * Server-Sent Events. We accumulate a rolling history of `MAX_SAMPLES`
 * snapshots so charts can draw without a full reload.
 *
 * Reconnects with exponential backoff when the connection drops. Surfaces
 * connection state to the consumer so the UI can render dead/live badges.
 */

'use client'

import { useEffect, useMemo, useRef, useState } from 'react'

import { isDemoMode, mockBenchmarkSnapshots } from '@/lib/demo-data'

export type BenchmarkPhase =
  | 'unspecified'
  | 'queued'
  | 'ramping'
  | 'holding'
  | 'draining'
  | 'complete'
  | 'cancelled'
  | 'failed'

const PHASE_LOOKUP: Record<number, BenchmarkPhase> = {
  0: 'unspecified',
  1: 'queued',
  2: 'ramping',
  3: 'holding',
  4: 'draining',
  5: 'complete',
  6: 'cancelled',
  7: 'failed',
}

export type BenchmarkSnapshot = {
  benchmarkId: string
  tsMs: number
  elapsedMs: number
  phase: BenchmarkPhase
  sentTotal: number
  ackedTotal: number
  erroredTotal: number
  currentRps: number
  targetRps: number
  p50Ns: number
  p90Ns: number
  p99Ns: number
  p999Ns: number
  maxNs: number
  correctnessScore: number
  compositeScore: number
  throughputScore: number
  latencyScore: number
  penaltyScore: number
  traceId: string
  /**
   * Kernel-side TCP latency, observed by the bot-worker's eBPF kprobe.
   * `kernelSamples === 0` means the probe wasn't reporting (build flag
   * off, kernel too old, or no samples in this window) and consumers
   * should hide the kernel overlay on the latency chart.
   */
  kernelP50Ns: number
  kernelP99Ns: number
  kernelP999Ns: number
  kernelSamples: number
}

type WireSnapshot = {
  benchmark_id: string
  ts_ns: number
  elapsed_ms: number
  phase: number
  sent_total: number
  acked_total: number
  errored: number
  current_rps: number
  target_rps: number
  latency: {
    p50_ns: number
    p90_ns: number
    p99_ns: number
    p999_ns: number
    max_ns: number
  }
  correctness_score: number
  composite_score: number
  throughput_score?: number   // optional for back-compat with older gateways
  latency_score?: number
  penalty_score?: number
  trace_id?: string
  /** Present on gateways ≥ Phase 1.2; absent on older builds. */
  kernel_latency?: {
    p50_ns: number
    p99_ns: number
    p999_ns: number
    samples: number
  }
}

export type ConnectionState = 'connecting' | 'open' | 'closed' | 'error'

const API_BASE =
  process.env.NEXT_PUBLIC_API_GATEWAY_URL ?? 'http://localhost:8080'

const MAX_SAMPLES = 600   // 10 minutes at 1Hz; ample for live charts.

export function useBenchmarkStream(benchmarkId: string | undefined) {
  const [latest, setLatest] = useState<BenchmarkSnapshot | null>(null)
  const [history, setHistory] = useState<BenchmarkSnapshot[]>([])
  const [state, setState] = useState<ConnectionState>('connecting')

  const esRef = useRef<EventSource | null>(null)
  const reconnectAt = useRef(500)

  useEffect(() => {
    if (!benchmarkId) {
      setLatest(null)
      setHistory([])
      setState('connecting')
      return undefined
    }

    setLatest(null)
    setHistory([])
    setState('connecting')

    // Demo mode short-circuit — no network calls; emit a deterministic
    // sequence so the UI looks alive during reviews.
    if (isDemoMode()) {
      setState('open')
      let i = 0
      const samples = mockBenchmarkSnapshots(benchmarkId)
      const tick = setInterval(() => {
        // samples.length is set inside mockBenchmarkSnapshots and is
        // always ≥ 1; the indexed read still widens to `T | undefined`
        // under noUncheckedIndexedAccess. Bind, narrow, then spread —
        // a partial spread would emit a BenchmarkSnapshot with every
        // field optional and trip TS2739 on the kernel* required keys.
        const sample = samples[i % samples.length]
        if (!sample) return
        const stamped: BenchmarkSnapshot = { ...sample, tsMs: Date.now() }
        setLatest(stamped)
        setHistory((prev) => trimSamples([...prev, stamped]))
        i++
      }, 250)
      return () => clearInterval(tick)
    }

    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | null = null

    const connect = () => {
      if (cancelled) return
      setState('connecting')
      const url = `${API_BASE}/v1/benchmarks/${encodeURIComponent(benchmarkId)}/stream`
      const es = new EventSource(url, { withCredentials: false })
      esRef.current = es

      es.onopen = () => {
        if (cancelled) return
        setState('open')
        reconnectAt.current = 500
      }

      es.onmessage = (ev) => {
        try {
          const wire = JSON.parse(ev.data as string) as WireSnapshot
          const snap = toSnapshot(wire)
          setLatest(snap)
          setHistory((prev) => trimSamples([...prev, snap]))
        } catch {
          /* ignore malformed payloads */
        }
      }

      es.onerror = () => {
        setState('error')
        es.close()
        if (cancelled) return
        const delay = Math.min(reconnectAt.current, 5_000)
        reconnectAt.current = Math.min(reconnectAt.current * 2, 5_000)
        timer = setTimeout(connect, delay)
      }
    }

    connect()
    return () => {
      cancelled = true
      if (timer) clearTimeout(timer)
      esRef.current?.close()
    }
  }, [benchmarkId])

  return useMemo(
    () => ({ latest, history, state }),
    [latest, history, state],
  )
}

/* -------------------------------------------------------------------------- */

function toSnapshot(w: WireSnapshot): BenchmarkSnapshot {
  return {
    benchmarkId: w.benchmark_id,
    tsMs: Math.floor(w.ts_ns / 1_000_000),
    elapsedMs: w.elapsed_ms,
    phase: PHASE_LOOKUP[w.phase] ?? 'unspecified',
    sentTotal: w.sent_total ?? 0,
    ackedTotal: w.acked_total ?? 0,
    erroredTotal: w.errored ?? 0,
    currentRps: w.current_rps ?? 0,
    targetRps: w.target_rps ?? 0,
    p50Ns: w.latency?.p50_ns ?? 0,
    p90Ns: w.latency?.p90_ns ?? 0,
    p99Ns: w.latency?.p99_ns ?? 0,
    p999Ns: w.latency?.p999_ns ?? 0,
    maxNs: w.latency?.max_ns ?? 0,
    correctnessScore: w.correctness_score ?? 0,
    compositeScore: w.composite_score ?? 0,
    // Back-solve from composite + correctness when the gateway omits the
    // sub-scores (older deployments). The back-solved values are approximate
    // but keep the modal renderable on day-zero of any rollout.
    throughputScore: w.throughput_score ?? estimateThroughputScore(w),
    latencyScore:    w.latency_score    ?? estimateLatencyScore(w),
    penaltyScore:    w.penalty_score    ?? 0,
    traceId:         w.trace_id         ?? '',
    kernelP50Ns:     w.kernel_latency?.p50_ns  ?? 0,
    kernelP99Ns:     w.kernel_latency?.p99_ns  ?? 0,
    kernelP999Ns:    w.kernel_latency?.p999_ns ?? 0,
    kernelSamples:   w.kernel_latency?.samples ?? 0,
  }
}

function estimateThroughputScore(w: WireSnapshot): number {
  if (!w.target_rps || w.target_rps === 0) return 0
  return 100 * Math.min(1, (w.current_rps ?? 0) / w.target_rps)
}

function estimateLatencyScore(w: WireSnapshot): number {
  const p99_us = (w.latency?.p99_ns ?? 0) / 1000
  const baseline_us = 30
  if (p99_us <= baseline_us) return 100
  return Math.max(0, 100 - 100 * (p99_us - baseline_us) / baseline_us)
}

function trimSamples(s: BenchmarkSnapshot[]): BenchmarkSnapshot[] {
  if (s.length <= MAX_SAMPLES) return s
  return s.slice(s.length - MAX_SAMPLES)
}
