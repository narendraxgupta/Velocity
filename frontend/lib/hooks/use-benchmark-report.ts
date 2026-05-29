/**
 * useBenchmarkReport — fetches the final report for a benchmark.
 *
 * Unlike useBenchmarkStream() (which is the live SSE feed), this hook pulls
 * the post-hoc aggregated report exposed at `GET /v1/benchmarks/:id`. It is
 * the only place the *cliff-finder* result lands today, because the cliff is
 * computed once at finalize time and stamped onto BenchmarkReport.
 *
 * Polling strategy: we hit the endpoint once on mount and again every 5s
 * until the report contains a non-zero finished_at_ns (i.e. the controller
 * has finalized the benchmark). After that we stop polling.
 */

'use client'

import { useEffect, useRef, useState } from 'react'

import { apiFetch } from '@/lib/api/client'

export type BenchmarkReport = {
  benchmark_id:      string
  submission_id:     string
  started_at_ns:     number
  finished_at_ns:    number
  total_orders:      number
  total_acked:       number
  total_errored:     number
  peak_rps_observed: number
  sustained_rps:     number
  latency: {
    p50_ns:  number
    p90_ns:  number
    p99_ns:  number
    p999_ns: number
    max_ns:  number
  }
  scores: {
    throughput:  number
    latency:     number
    correctness: number
    composite:   number
  }
  trace_id: string
  cliff?: {
    detected:   boolean
    rps:        number
    lower_rps:  number
    upper_rps:  number
    confidence: number
    reason:     string
  }
  /** Multi-venue breakdown. Empty array on single-symbol runs. */
  venues?: VenueResult[]
  /** Cross-venue p99 spread (max − min) in nanoseconds. Zero in single-venue. */
  cross_venue_skew_ns?: number

  /**
   * Submitter-supplied metadata. The report endpoint enriches with these
   * once the upload manifest is matched to the run; before then they are
   * absent and the UI should fall back to defaults (cpp / BINARY / "").
   */
  language?: string
  kind?:     string
  source?:   string

  /**
   * Aggregated execution-quality metrics published by the validator at
   * finalize time. The live exec-quality panel reads from a separate
   * Redis key but we surface the post-hoc snapshot here so consumers
   * (critique panel) don't need a second hook.
   */
  execQuality?: {
    slippage_bps?:                 number
    implementation_shortfall_bps?: number
    reversion_bps?:                number
  }
}

export type VenueResult = {
  venue_id:    string
  symbol:      string
  sent_total:  number
  acked_total: number
  p50_ns:      number
  p99_ns:      number
  p999_ns:     number
}

export function useBenchmarkReport(benchmarkId: string | undefined) {
  const [report, setReport] = useState<BenchmarkReport | null>(null)
  const [error,  setError]  = useState<string | null>(null)
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null)

  useEffect(() => {
    if (!benchmarkId) {
      setReport(null)
      setError(null)
      return undefined
    }
    setReport(null)
    setError(null)
    let cancelled = false

    const poll = async () => {
      try {
        const res = await apiFetch(
          `/v1/benchmarks/${encodeURIComponent(benchmarkId)}`,
          { cache: 'no-store' },
        )
        if (!res.ok) {
          // 404 just means the benchmark hasn't been registered yet; back off
          // and try again rather than surfacing it as an error.
          if (res.status !== 404 && !cancelled) {
            setError(`HTTP ${res.status}`)
          }
          schedule()
          return
        }
        const json = (await res.json()) as BenchmarkReport
        if (cancelled) return
        setReport(json)
        setError(null)
        // Keep polling until the controller marks the run finished.
        if (!json.finished_at_ns || json.finished_at_ns === 0) {
          schedule()
        }
      } catch (e) {
        if (cancelled) return
        setError((e as Error).message)
        schedule()
      }
    }

    const schedule = () => {
      if (cancelled) return
      timer.current = setTimeout(poll, 5_000)
    }

    poll()
    return () => {
      cancelled = true
      if (timer.current) clearTimeout(timer.current)
    }
  }, [benchmarkId])

  return { report, error }
}
