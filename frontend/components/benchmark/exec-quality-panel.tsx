/**
 * ExecQualityPanel — surfaces slippage / IS / reversion stats from the
 * correctness validator's execution-quality tracker.
 *
 * What each metric means (industry framing):
 *
 *   slippage_bps  — (executed_vwap − decision_mid) / decision_mid × 1e4
 *                  Positive: paid up for buys (or received less for sells).
 *                  Negative: price-improved relative to the mid at order arrival.
 *
 *   IS_bps        — same arithmetic; semantically "the cost you paid for
 *                  not being able to trade at the decision price". Reported
 *                  alongside slippage so judges see both standard framings.
 *
 *   reversion_bps — (executed_vwap − post_mid_5s) / executed_vwap × 1e4
 *                  Positive: price moved AGAINST your direction after you
 *                  printed (good — you "caught the move").
 *                  Negative: adverse selection — you bought before the dip.
 *
 *   p50 / p95     — robust percentiles over the most recent 4096 prints,
 *                  reservoir-sampled. Less noisy than mean for fat-tailed
 *                  distributions.
 */

'use client'

import { useEffect, useState } from 'react'

import {
  Panel,
  PanelBody,
  PanelDescription,
  PanelHeader,
  PanelTitle,
} from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'

interface ExecQuality {
  orders_observed:    number
  fully_marked:       number
  slippage_mean_bps:  number
  slippage_p50_bps:   number
  slippage_p95_bps:   number
  is_mean_bps:        number
  is_p50_bps:         number
  is_p95_bps:         number
  reversion_mean_bps: number
  reversion_p50_bps:  number
  reversion_p95_bps:  number
}

interface Props {
  submissionId: string
}

export function ExecQualityPanel({ submissionId }: Props) {
  const [data, setData] = useState<ExecQuality | null>(null)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    if (!submissionId) return undefined
    let cancelled = false
    const fetchOnce = async () => {
      try {
        const resp = await apiFetch(
          `/v1/submissions/${encodeURIComponent(submissionId)}/exec-quality`,
          { cache: 'no-store' },
        )
        if (resp.status === 404) {
          // No data yet — keep polling silently, this is normal early in a run.
          if (!cancelled) setError(null)
          return
        }
        if (!resp.ok) {
          if (!cancelled) setError(`HTTP ${resp.status}`)
          return
        }
        const body = (await resp.json()) as ExecQuality
        if (cancelled) return
        setData(body)
        setError(null)
      } catch (err) {
        if (!cancelled) setError((err as Error).message)
      }
    }
    fetchOnce()
    const id = setInterval(fetchOnce, 2_000)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [submissionId])

  if (!data && !error) return null  // first render with no data — quiet

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Execution quality</PanelTitle>
        <PanelDescription>
          Slippage, implementation shortfall, and post-trade reversion measured
          against the reference orderbook&apos;s mid. Computed on the validator
          over the last 4 096 prints. Reservoir-sampled, so the percentiles
          are stable even for high-rate runs.
        </PanelDescription>
      </PanelHeader>

      <PanelBody>
        {error ? (
          <div className="rounded border border-signal-ask/40 bg-signal-ask/5 p-3 font-mono text-2xs text-signal-ask">
            {error}
          </div>
        ) : data ? (
          <>
            <div className="mb-3 font-mono text-2xs uppercase tracking-widest text-muted-foreground">
              {data.orders_observed.toLocaleString()} orders observed ·{' '}
              {data.fully_marked.toLocaleString()} fully marked
            </div>
            <div className="grid grid-cols-1 gap-4 md:grid-cols-3">
              <MetricBlock
                title="Slippage"
                tone={data.slippage_p50_bps > 0 ? 'cost' : 'gain'}
                mean={data.slippage_mean_bps}
                p50={data.slippage_p50_bps}
                p95={data.slippage_p95_bps}
                framing="(vwap − decision_mid) / decision_mid × 1e4"
              />
              <MetricBlock
                title="Implementation shortfall"
                tone={data.is_p50_bps > 0 ? 'cost' : 'gain'}
                mean={data.is_mean_bps}
                p50={data.is_p50_bps}
                p95={data.is_p95_bps}
                framing="cost of execution vs decision price"
              />
              <MetricBlock
                title="5s reversion"
                tone={data.reversion_p50_bps > 0 ? 'gain' : 'cost'}
                mean={data.reversion_mean_bps}
                p50={data.reversion_p50_bps}
                p95={data.reversion_p95_bps}
                framing="(vwap − post_mid_5s) / vwap × 1e4"
              />
            </div>
          </>
        ) : null}
      </PanelBody>
    </Panel>
  )
}

function MetricBlock({
  title, mean, p50, p95, framing, tone,
}: {
  title: string
  mean: number
  p50: number
  p95: number
  framing: string
  tone: 'cost' | 'gain'
}) {
  const color = tone === 'cost' ? 'text-signal-ask' : 'text-signal-live'
  return (
    <div className="rounded-md border border-border bg-card/50 p-3">
      <div className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
        {title}
      </div>
      <div className={`mt-1 font-display text-2xl font-semibold tracking-tight ${color}`}>
        {formatBps(p50)}
      </div>
      <div className="mt-1 font-mono text-2xs text-muted-foreground">
        mean {formatBps(mean)} · p95 {formatBps(p95)}
      </div>
      <div className="mt-2 font-mono text-2xs text-muted-foreground/70">
        {framing}
      </div>
    </div>
  )
}

function formatBps(v: number): string {
  if (!isFinite(v) || v === 0) return '0bps'
  const sign = v > 0 ? '+' : ''
  if (Math.abs(v) < 1) return `${sign}${v.toFixed(2)}bps`
  if (Math.abs(v) < 100) return `${sign}${v.toFixed(1)}bps`
  return `${sign}${v.toFixed(0)}bps`
}
