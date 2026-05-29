/**
 * RegressionBadge — surfaces the KS-test verdict from anomaly-detector
 * on the submission detail page.
 *
 * Fetched on mount from `/v1/submissions/{id}/regression`. When the
 * endpoint returns 404 we silently render nothing — this is a
 * "value-add" signal and a first-time run with no baseline should not
 * draw attention to itself.
 *
 * Three states:
 *   improved → green pill with "−Xµs at p99 (p=...)"
 *   stable   → muted: nothing to see
 *   regressed → red pill with "+Xµs at p99 (p=...)"
 */

'use client'

import { useEffect, useState } from 'react'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { cn } from '@/lib/utils'

type Verdict = 'improved' | 'stable' | 'regressed'

interface Regression {
  submission_id:  string
  baseline_ts_ms: number
  current_ts_ms:  number
  n_baseline:     number
  n_current:      number
  ks_stat:        number
  ks_pvalue:      number
  p50_delta_ns:   number
  p99_delta_ns:   number
  p999_delta_ns:  number
  regression:     Verdict
  human_summary:  string
}

export function RegressionBadge({ submissionId }: { submissionId: string }) {
  const [reg, setReg] = useState<Regression | null>(null)
  const [missing, setMissing] = useState(false)

  useEffect(() => {
    if (!submissionId) return undefined
    setReg(null)
    setMissing(false)
    let cancelled = false
    apiFetch(`/v1/submissions/${encodeURIComponent(submissionId)}/regression`, {
      cache: 'no-store',
    })
      .then(async (r) => {
        if (cancelled) return
        if (r.status === 404) {
          setMissing(true)
          return
        }
        if (!r.ok) return
        const body = (await r.json()) as Regression
        setReg(body)
      })
      .catch(() => { /* network errors are non-fatal */ })
    return () => { cancelled = true }
  }, [submissionId])

  if (missing || !reg) return null

  const tone =
    reg.regression === 'regressed'
      ? 'border-signal-ask/40 bg-signal-ask/10 text-signal-ask'
      : reg.regression === 'improved'
      ? 'border-signal-live/40 bg-signal-live/10 text-signal-live'
      : 'border-border bg-surface-elevated text-muted-foreground'

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Run-over-run latency</PanelTitle>
        <PanelDescription>
          Two-sample Kolmogorov–Smirnov on the HdrHistogram buckets vs the previous
          run on this submission. Regression threshold: p &lt; 0.01 and Δp99 &gt; 5µs.
        </PanelDescription>
      </PanelHeader>
      <PanelBody className="flex flex-wrap items-center gap-4">
        <span
          className={cn(
            'inline-flex items-center gap-1 rounded-sm border px-2 py-1 font-mono text-2xs font-semibold uppercase tracking-wider',
            tone,
          )}
        >
          {reg.regression}
        </span>
        <span className="font-mono text-sm">
          {formatDelta(reg.p50_delta_ns)} at p50 ·{' '}
          {formatDelta(reg.p99_delta_ns)} at p99 ·{' '}
          {formatDelta(reg.p999_delta_ns)} at p999
        </span>
        <span className="font-mono text-2xs text-muted-foreground">
          KS p = {reg.ks_pvalue.toExponential(2)} ·{' '}
          n_base = {reg.n_baseline.toLocaleString()} ·{' '}
          n_curr = {reg.n_current.toLocaleString()}
        </span>
        <span className="ml-auto text-sm text-muted-foreground">{reg.human_summary}</span>
      </PanelBody>
    </Panel>
  )
}

function formatDelta(ns: number): string {
  const sign = ns >= 0 ? '+' : '−'
  const us = Math.abs(ns) / 1000
  if (us >= 1000) return `${sign}${(us / 1000).toFixed(2)}ms`
  return `${sign}${us.toFixed(1)}µs`
}
