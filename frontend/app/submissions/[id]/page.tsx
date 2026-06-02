/**
 * Submission detail / live-run console.
 *
 * Renders the gateway's per-benchmark SSE stream:
 *   - phase pill (queued -> ramping -> holding -> draining -> complete)
 *   - live composite score + sub-scores (throughput / latency / correctness)
 *   - RPS chart with throughput-cliff marker
 *   - p50/p90/p99/p999 latency chart
 *   - sent / acked / errored counters
 *   - links to /submissions/[id]/mismatches and /submissions/[id]/build
 */

'use client'

import Link from 'next/link'
import { useParams } from 'next/navigation'
import { useEffect, useMemo, useState } from 'react'

import { LatencyChart } from '@/components/charts/latency-chart'
import { LatencyHeatmap } from '@/components/charts/latency-heatmap'
import { CliffCard } from '@/components/benchmark/cliff-card'
import { PhasePill } from '@/components/benchmark/phase-pill'
import { RpsChart } from '@/components/benchmark/rps-chart'
import { ExecQualityPanel } from '@/components/benchmark/exec-quality-panel'
import { VenueBreakdown } from '@/components/benchmark/venue-breakdown'
import { CritiquePanel } from '@/components/critique/critique-panel'
import { RegressionBadge } from '@/components/benchmark/regression-badge'
import { AdaptiveNextRun } from '@/components/benchmark/adaptive-next-run'
import { MetricCard } from '@/components/metric-card'
import { ScoreBreakdown } from '@/components/scoring/score-breakdown'
import { ShareButton } from '@/components/share/share-button'
import { PcapDownloadLink } from '@/components/submissions/pcap-download-link'
import {
  Panel,
  PanelBody,
  PanelDescription,
  PanelHeader,
  PanelTitle,
} from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { useBenchmarkReport } from '@/lib/hooks/use-benchmark-report'
import { useBenchmarkStream } from '@/lib/hooks/use-benchmark-stream'
import {
  formatLatencyNs,
  formatRelativeMs,
  formatRps,
  formatScore,
  middleTruncate,
} from '@/lib/utils'

const ADMIN_RUNS_KEY = 'velocity:admin:runs'

/**
 * Resolve the Jaeger trace URL. In a Codespace/github.dev each forwarded port
 * is its own subdomain (`…-3000.app.github.dev`), so we derive the Jaeger UI
 * host (`…-16686`) from the current location rather than the hardcoded
 * `localhost:16686` that only works on a local dev box. NEXT_PUBLIC_JAEGER_URL
 * overrides everything.
 */
function traceUrl(traceId: string): string {
  const configured = process.env.NEXT_PUBLIC_JAEGER_URL
  if (configured) return `${configured.replace(/\/$/, '')}/trace/${traceId}`
  if (typeof window !== 'undefined') {
    const m = window.location.host.match(/^(.*)-(\d+)(\.app\.github\.dev)$/)
    if (m) {
      return `${window.location.protocol}//${m[1]}-16686${m[3]}/trace/${traceId}`
    }
  }
  return `http://localhost:16686/trace/${traceId}`
}

export default function SubmissionDetailPage() {
  const params = useParams<{ id: string }>()
  const routeId = params?.id ?? ''
  const routeIsBenchmark = routeId.startsWith('BM-')

  const [benchmarkId, setBenchmarkId] = useState<string | undefined>(
    routeIsBenchmark ? routeId : undefined,
  )

  useEffect(() => {
    if (routeIsBenchmark) {
      setBenchmarkId(routeId)
      return undefined
    }
    let cancelled = false
    const resolve = async () => {
      try {
        const res = await apiFetch(
          `/v1/submissions/${encodeURIComponent(routeId)}/score`,
          { cache: 'no-store' },
        )
        if (res.ok) {
          const data = (await res.json()) as { benchmark_id?: string }
          const bid = data.benchmark_id?.trim()
          if (bid && !cancelled) {
            setBenchmarkId(bid)
            return
          }
        }
      } catch {
        /* retry on next tick */
      }
      try {
        const raw = window.localStorage.getItem(ADMIN_RUNS_KEY)
        if (raw) {
          const runs = JSON.parse(raw) as Array<{
            submissionId: string
            benchmarkId: string
            status: string
          }>
          const hit = runs.find(
            (r) => r.submissionId === routeId && r.status === 'running',
          )
          if (hit?.benchmarkId && !cancelled) setBenchmarkId(hit.benchmarkId)
        }
      } catch {
        /* ignore corrupt local history */
      }
    }
    setBenchmarkId(undefined)
    void resolve()
    const t = setInterval(() => void resolve(), 2_000)
    return () => {
      cancelled = true
      clearInterval(t)
    }
  }, [routeId, routeIsBenchmark])

  const { latest, history, state } = useBenchmarkStream(benchmarkId)
  const { report } = useBenchmarkReport(benchmarkId)

  const submissionId = routeIsBenchmark
    ? (report?.submission_id ?? '')
    : routeId
  const [breakdownOpen, setBreakdownOpen] = useState(false)

  const samples = useMemo(
    () =>
      history.map((s) => ({
        tsMs: s.tsMs,
        p50Ns: s.p50Ns,
        p90Ns: s.p90Ns,
        p99Ns: s.p99Ns,
        p999Ns: s.p999Ns,
        kernelP50Ns: s.kernelP50Ns,
        kernelP99Ns: s.kernelP99Ns,
        kernelSamples: s.kernelSamples,
      })),
    [history],
  )

  const heatmapSamples = useMemo(
    () =>
      history.map((s) => ({
        tsMs: s.tsMs,
        p50Ns: s.p50Ns,
        p90Ns: s.p90Ns,
        p99Ns: s.p99Ns,
        p999Ns: s.p999Ns,
        maxNs: s.maxNs,
      })),
    [history],
  )

  const rpsSamples = useMemo(
    () =>
      history.map((s) => ({
        tsMs: s.tsMs,
        currentRps: s.currentRps,
        targetRps: s.targetRps,
      })),
    [history],
  )

  return (
    <div className="container space-y-6 py-8">
      <header className="flex flex-wrap items-end justify-between gap-3">
        <div>
          <span className="label-eyebrow">submission / benchmark · live</span>
          <h1 className="font-display text-2xl font-semibold tracking-tight">
            {middleTruncate(routeId, 28)}
          </h1>
          <p className="text-sm text-muted-foreground">
            Live state, refreshed every ~250 ms. Connection:{' '}
            <span className="font-mono text-foreground">{state}</span>
          </p>
        </div>
        <div className="flex items-center gap-2">
          <PhasePill phase={latest?.phase ?? 'unspecified'} />
          {latest?.traceId && (
            <a
              href={traceUrl(latest.traceId)}
              target="_blank"
              rel="noopener noreferrer"
              className="text-sm font-medium text-accent hover:underline"
              title="Open the full benchmark trace in Jaeger"
            >
              View trace →
            </a>
          )}
          <Link
            href={`/submissions/${encodeURIComponent(submissionId || routeId)}/mismatches`}
            className="text-sm font-medium text-accent hover:underline"
          >
            View mismatches →
          </Link>
          <Link
            href={`/submissions/${encodeURIComponent(submissionId || routeId)}/profile`}
            className="text-sm font-medium text-accent hover:underline"
          >
            Flamegraph →
          </Link>
          <Link
            href={`/submissions/${encodeURIComponent(submissionId || routeId)}/orderbook`}
            className="text-sm font-medium text-accent hover:underline"
          >
            Orderbook →
          </Link>
          <Link
            href={`/submissions/${encodeURIComponent(submissionId || routeId)}/build`}
            className="text-sm font-medium text-accent hover:underline"
          >
            Build log →
          </Link>
          <PcapDownloadLink benchmarkId={latest?.benchmarkId ?? null} />
          {latest?.benchmarkId && (
            <ShareButton
              submissionId={submissionId || routeId}
              benchmarkId={latest.benchmarkId}
            />
          )}
        </div>
      </header>

      <section className="grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-4">
        <button
          type="button"
          onClick={() => latest && setBreakdownOpen(true)}
          disabled={!latest}
          className="text-left transition-transform focus:outline-none focus-visible:ring-2 focus-visible:ring-accent disabled:cursor-default disabled:opacity-100"
          title={latest ? 'Click to explain this score' : 'Waiting for first snapshot'}
        >
          <MetricCard
            label="Composite · click to explain"
            value={latest ? formatScore(latest.compositeScore) : '—'}
            unit="/100"
            hint="0.4 × tps + 0.35 × lat + 0.25 × corr − 0.05 × pen"
            tone="live"
            trend="up"
          />
        </button>
        <MetricCard
          label="Current RPS"
          value={latest ? formatRps(latest.currentRps) : '—'}
          unit="req/s"
          hint={latest ? `of target ${formatRps(latest.targetRps)}` : 'streaming…'}
          tone="ask"
          trend="up"
        />
        <MetricCard
          label="p99 latency"
          value={latest ? formatLatencyNs(latest.p99Ns) : '—'}
          hint={latest ? `p999 ${formatLatencyNs(latest.p999Ns)}` : 'streaming…'}
          tone="warn"
          trend="flat"
        />
        <MetricCard
          label="Correctness"
          value={latest ? formatScore(latest.correctnessScore) : '—'}
          unit="/100"
          hint={
            latest
              ? `${latest.ackedTotal.toLocaleString()} acked · ${latest.erroredTotal.toLocaleString()} err`
              : 'streaming…'
          }
          tone="info"
        />
      </section>

      <section className="grid grid-cols-1 gap-3 xl:grid-cols-2">
        <Panel>
          <PanelHeader>
            <PanelTitle>Throughput</PanelTitle>
            <PanelDescription>
              Offered vs sustained RPS. The dashed vertical line, if present, marks the
              throughput cliff — the moment sustained dropped below 80 % of offered for
              ≥ 750 ms.
            </PanelDescription>
          </PanelHeader>
          <PanelBody>
            <RpsChart samples={rpsSamples} />
          </PanelBody>
        </Panel>
        <Panel>
          <PanelHeader>
            <PanelTitle>Latency percentiles</PanelTitle>
            <PanelDescription>
              Log-scale p50 / p90 / p99 / p999, intended-time corrected. Honest tail
              numbers — coordinated omission cannot hide here.
            </PanelDescription>
          </PanelHeader>
          <PanelBody>
            <LatencyChart samples={samples} />
          </PanelBody>
        </Panel>
      </section>

      {report?.cliff && <CliffCard cliff={report.cliff} />}

      <RegressionBadge submissionId={submissionId || routeId} />

      <AdaptiveNextRun submissionId={submissionId || routeId} report={report} />

      <VenueBreakdown
        venues={report?.venues}
        crossVenueSkewNs={report?.cross_venue_skew_ns}
      />

      <ExecQualityPanel submissionId={submissionId || routeId} />

      <CritiquePanel
        context={
          latest
            ? {
                submissionId: submissionId || routeId,
                language: report?.language ?? 'cpp',
                kind: report?.kind ?? 'BINARY',
                // The detail page doesn't ship the source itself; the
                // critique-service falls back to "(source unavailable)"
                // when this is empty, which the LLM handles gracefully.
                source: report?.source ?? '',
                score: latest.compositeScore,
                throughputRps: latest.currentRps,
                p50LatencyNs: latest.p50Ns,
                p99LatencyNs: latest.p99Ns,
                p999LatencyNs: latest.p999Ns,
                correctnessRatio: latest.correctnessScore / 100,
                orderRejectRate:
                  latest.sentTotal > 0
                    ? latest.erroredTotal / latest.sentTotal
                    : 0,
                slippageBps: report?.execQuality?.slippage_bps,
                isBps: report?.execQuality?.implementation_shortfall_bps,
                reversionBps: report?.execQuality?.reversion_bps,
                kernelUserspaceSkewNs:
                  latest.kernelP99Ns && latest.p99Ns
                    ? latest.p99Ns - latest.kernelP99Ns
                    : undefined,
              }
            : null
        }
      />

      <Panel>
        <PanelHeader>
          <PanelTitle>Latency density heatmap</PanelTitle>
          <PanelDescription>
            Time × log-latency density. Hot cells reveal where the bulk of orders
            cluster; bright streaks at the top expose tail-latency events that the
            line chart smooths away. The y-axis spans 1µs to 10s across 28 log-spaced
            buckets.
          </PanelDescription>
        </PanelHeader>
        <PanelBody>
          <LatencyHeatmap samples={heatmapSamples} />
        </PanelBody>
      </Panel>

      <section className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <MetricCard label="Sent"      value={latest?.sentTotal?.toLocaleString() ?? '—'}    hint="cumulative orders dispatched" />
        <MetricCard label="Acked"     value={latest?.ackedTotal?.toLocaleString() ?? '—'}   hint="cumulative acks observed" />
        <MetricCard
          label="Errored"
          value={latest?.erroredTotal?.toLocaleString() ?? '—'}
          hint="includes timeouts + 5xx"
          tone={latest && latest.erroredTotal > 0 ? 'warn' : 'neutral'}
        />
      </section>

      <Panel>
        <PanelHeader>
          <PanelTitle>Telemetry session</PanelTitle>
          <PanelDescription>
            {latest
              ? `Elapsed ${(latest.elapsedMs / 1000).toFixed(1)}s · last update ${
                  // history.length>0 implies history[last] is defined, but
                  // noUncheckedIndexedAccess can't follow the length check
                  // through. Bind, then test for undefined explicitly.
                  (() => {
                    const last = history[history.length - 1]
                    return last ? formatRelativeMs(Date.now() - last.tsMs) : '—'
                  })()
                }`
              : 'Waiting for first snapshot…'}
          </PanelDescription>
        </PanelHeader>
      </Panel>

      <ScoreBreakdown
        open={breakdownOpen}
        onOpenChange={setBreakdownOpen}
        score={
          latest
            ? {
                submissionId: submissionId || routeId,
                composite:    latest.compositeScore,
                throughput:   latest.throughputScore,
                latency:      latest.latencyScore,
                correctness:  latest.correctnessScore,
                penalty:      latest.penaltyScore ?? 0,
                sustainedRps: latest.currentRps,
                targetRps:    latest.targetRps,
                p99Ns:        latest.p99Ns,
              }
            : null
        }
      />
    </div>
  )
}
