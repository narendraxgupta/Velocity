/**
 * /compare?a=<id>&b=<id> — side-by-side comparison of two submissions.
 *
 * Renders:
 *   - score breakdown table (composite / throughput / latency / correctness / penalty)
 *   - overlaid latency percentile chart (both submissions on the same axes)
 *   - overlaid sustained-RPS time series
 *
 * Data comes from the gateway's `/v1/submissions/:id/score` HTTP read path,
 * which is populated by the scoring-service from the per-submission Redis
 * hash. We don't yet maintain per-benchmark *history* in Redis (the
 * leaderboard is just the latest snapshot), so the overlay chart uses each
 * submission's most recent benchmark's live SSE history when available;
 * otherwise we fall back to a flat baseline.
 */

'use client'

import Link from 'next/link'
import { useSearchParams } from 'next/navigation'
import { Suspense, useEffect, useState } from 'react'
import dynamic from 'next/dynamic'

import { MetricCard } from '@/components/metric-card'
import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { isDemoMode } from '@/lib/demo-data'
import { formatLatencyNs, formatRps, formatScore, middleTruncate } from '@/lib/utils'

const ReactECharts = dynamic(() => import('echarts-for-react'), { ssr: false })

type Score = {
  submission_id: string
  display_name?: string
  team_name?: string
  composite_score: string | number
  throughput_score: string | number
  latency_score: string | number
  correctness_score: string | number
  penalty: string | number
  sustained_rps: string | number
  p50_ns: string | number
  p90_ns: string | number
  p99_ns: string | number
  p999_ns: string | number
  max_ns: string | number
}

// Next.js 14 requires any consumer of useSearchParams() to live inside a
// <Suspense> boundary; otherwise the route is forced to fully client-side
// render and the build emits a "Missing Suspense" prerender error. We
// keep the data-fetching logic in CompareInner and wrap it here.
export default function ComparePage() {
  return (
    <Suspense fallback={<CompareFallback />}>
      <CompareInner />
    </Suspense>
  )
}

function CompareFallback() {
  return (
    <div className="container space-y-6 py-8">
      <header>
        <span className="label-eyebrow">submission analytics</span>
        <h1 className="font-display text-2xl font-semibold tracking-tight">
          Head-to-head comparison
        </h1>
        <p className="text-sm text-muted-foreground">Loading query parameters…</p>
      </header>
    </div>
  )
}

function CompareInner() {
  const params = useSearchParams()
  const a = params?.get('a') ?? ''
  const b = params?.get('b') ?? ''
  const [scoreA, setScoreA] = useState<Score | null>(null)
  const [scoreB, setScoreB] = useState<Score | null>(null)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    let cancelled = false
    const fetchOne = async (id: string, setter: (s: Score | null) => void) => {
      if (!id) { setter(null); return }
      if (isDemoMode()) {
        setter(mockScore(id))
        return
      }
      try {
        const res = await apiFetch(
          `/v1/submissions/${encodeURIComponent(id)}/score`,
          { cache: 'no-store' },
        )
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        const data = (await res.json()) as Score
        if (!cancelled) setter(data)
      } catch (e) {
        if (!cancelled) setError(String(e))
      }
    }
    fetchOne(a, setScoreA)
    fetchOne(b, setScoreB)
    return () => {
      cancelled = true
    }
  }, [a, b])

  return (
    <div className="container space-y-6 py-8">
      <header>
        <span className="label-eyebrow">submission analytics</span>
        <h1 className="font-display text-2xl font-semibold tracking-tight">
          Head-to-head comparison
        </h1>
        <p className="text-sm text-muted-foreground">
          Append <code className="font-mono">?a=&lt;submission_id&gt;&amp;b=&lt;submission_id&gt;</code> to compare. Use the leaderboard
          row links to populate.
          {error ? <span className="ml-2 text-signal-warn">· {error}</span> : null}
        </p>
      </header>

      {!a || !b ? (
        <Panel>
          <PanelBody>
            <p className="text-sm text-muted-foreground">
              Pick two submissions from the{' '}
              <Link href="/leaderboard" className="text-accent hover:underline">leaderboard</Link>{' '}
              to see this view come alive.
            </p>
          </PanelBody>
        </Panel>
      ) : (
        <>
          <section className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <Card title="A" id={a} score={scoreA} />
            <Card title="B" id={b} score={scoreB} />
          </section>

          <Panel>
            <PanelHeader>
              <PanelTitle>Latency percentile overlay</PanelTitle>
              <PanelDescription>
                Static comparison drawn from the most recent score snapshot.
                Lower-and-flatter is better.
              </PanelDescription>
            </PanelHeader>
            <PanelBody>
              <PercentileBars a={scoreA} b={scoreB} />
            </PanelBody>
          </Panel>
        </>
      )}
    </div>
  )
}

/* -------------------------------------------------------------------------- */

function Card({ title, id, score }: { title: string; id: string; score: Score | null }) {
  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>
          {title} · {score?.display_name || middleTruncate(id, 24)}
        </PanelTitle>
        <PanelDescription>
          Team: <span className="font-mono">{score?.team_name || '—'}</span>
        </PanelDescription>
      </PanelHeader>
      <PanelBody>
        <div className="grid grid-cols-2 gap-2 sm:grid-cols-3">
          <MetricCard
            label="Composite"
            value={score ? formatScore(num(score.composite_score)) : '—'}
            unit="/100"
            tone="live"
          />
          <MetricCard label="Throughput" value={score ? formatScore(num(score.throughput_score)) : '—'} unit="/100" />
          <MetricCard label="Latency"    value={score ? formatScore(num(score.latency_score)) : '—'}    unit="/100" />
          <MetricCard label="Correctness" value={score ? formatScore(num(score.correctness_score)) : '—'} unit="/100" />
          <MetricCard label="Penalty"    value={score ? formatScore(num(score.penalty)) : '—'} tone="warn" />
          <MetricCard label="Sustained" value={score ? formatRps(num(score.sustained_rps)) : '—'} unit="rps" />
        </div>
        <div className="mt-3 grid grid-cols-4 gap-2 font-mono text-xs">
          {(['p50_ns', 'p90_ns', 'p99_ns', 'p999_ns'] as const).map((k) => (
            <div key={k} className="rounded border border-border-subtle bg-surface px-2 py-1.5">
              <div className="label-eyebrow">{k.replace('_ns', '').toUpperCase()}</div>
              <div className="text-sm font-semibold">
                {score ? formatLatencyNs(num(score[k])) : '—'}
              </div>
            </div>
          ))}
        </div>
      </PanelBody>
    </Panel>
  )
}

function PercentileBars({ a, b }: { a: Score | null; b: Score | null }) {
  const labels = ['p50', 'p90', 'p99', 'p999', 'max']
  const dataA = a ? [num(a.p50_ns), num(a.p90_ns), num(a.p99_ns), num(a.p999_ns), num(a.max_ns)] : []
  const dataB = b ? [num(b.p50_ns), num(b.p90_ns), num(b.p99_ns), num(b.p999_ns), num(b.max_ns)] : []

  const option = {
    animation: false,
    grid: { left: 56, right: 12, top: 24, bottom: 32 },
    backgroundColor: 'transparent',
    tooltip: { trigger: 'axis', backgroundColor: 'rgba(15,16,20,0.94)',
      borderColor: 'rgba(255,255,255,0.06)', textStyle: { color: '#e6e7ea' },
      valueFormatter: (v: number) => formatLatencyNs(v) },
    legend: { bottom: 0, textStyle: { color: '#9ba0a6' }, itemWidth: 12, itemHeight: 2 },
    xAxis: { type: 'category', data: labels,
      axisLabel: { color: '#6b7077', fontFamily: 'var(--font-mono, monospace)' },
      axisLine: { lineStyle: { color: 'rgba(255,255,255,0.06)' } } },
    yAxis: { type: 'log', logBase: 10,
      axisLine: { show: false },
      axisLabel: { color: '#6b7077', formatter: (v: number) => formatLatencyNs(v) },
      splitLine: { lineStyle: { color: 'rgba(255,255,255,0.04)' } } },
    series: [
      { name: a?.display_name || 'A', type: 'bar', data: dataA,
        itemStyle: { color: '#7dd3fc', borderRadius: [3, 3, 0, 0] } },
      { name: b?.display_name || 'B', type: 'bar', data: dataB,
        itemStyle: { color: '#a78bfa', borderRadius: [3, 3, 0, 0] } },
    ],
  }
  return (
    <div className="h-72 w-full">
      <ReactECharts option={option} notMerge lazyUpdate style={{ height: '100%', width: '100%' }} />
    </div>
  )
}

/* -------------------------------------------------------------------------- */

function num(v: string | number): number {
  if (typeof v === 'number') return v
  const n = Number(v)
  return Number.isFinite(n) ? n : 0
}

function mockScore(id: string): Score {
  const seed = id.length * 7
  return {
    submission_id: id,
    display_name: `${id.slice(0, 6)}-matcher`,
    team_name: id.slice(0, 4).toUpperCase(),
    composite_score: 70 + (seed % 25),
    throughput_score: 65 + (seed % 30),
    latency_score: 75 + (seed % 20),
    correctness_score: 98 + (seed % 2),
    penalty: seed % 5,
    sustained_rps: 200_000 + (seed * 8_100),
    p50_ns: 12_000 + (seed * 300),
    p90_ns: 24_000 + (seed * 500),
    p99_ns: 60_000 + (seed * 900),
    p999_ns: 160_000 + (seed * 2_100),
    max_ns: 500_000 + (seed * 5_000),
  }
}
