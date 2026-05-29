/**
 * VenueBreakdown — renders the per-venue p50/p99 table for cross-venue
 * benchmarks plus the headline cross-venue skew.
 *
 * The component hides itself entirely when `venues` is empty or missing —
 * single-symbol benchmarks should look identical to the pre-Phase-2.1
 * detail page, so we don't want a "no venues" placeholder taking up
 * vertical space.
 *
 * Skew interpretation:
 *
 *   - 0 ns          → "fallback" mode: the controller couldn't get
 *                     per-venue p99 from Redis and replicated the global
 *                     value across legs. Treat as "data not yet ready".
 *   - 1 ns – 10µs   → healthy: legs are running within scheduler jitter.
 *   - 10µs – 100µs  → mild: one venue starting to lag, watch the chart.
 *   - >100µs        → significant: judge-level signal that the engine
 *                     is queuing one venue behind another.
 */

'use client'

import {
  Panel,
  PanelBody,
  PanelDescription,
  PanelHeader,
  PanelTitle,
} from '@/components/ui/panel'

import type { VenueResult } from '@/lib/hooks/use-benchmark-report'

interface Props {
  venues?: VenueResult[]
  crossVenueSkewNs?: number
}

export function VenueBreakdown({ venues, crossVenueSkewNs }: Props) {
  if (!venues || venues.length === 0) return null

  // Largest-p99 wins the "lagging" highlight; smallest is the reference.
  const minP99 = venues.reduce((a, v) => Math.min(a, v.p99_ns), Number.POSITIVE_INFINITY)
  const maxP99 = venues.reduce((a, v) => Math.max(a, v.p99_ns), 0)

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Cross-venue breakdown</PanelTitle>
        <PanelDescription>
          Per-leg latency, sent/ack counts, and the headline cross-venue p99
          skew. A non-trivial skew (&gt; 100µs) is a strong signal that the
          engine under test is queuing one venue&apos;s order flow behind
          another.
        </PanelDescription>
      </PanelHeader>

      <PanelBody>
        <div className="mb-3 font-mono text-2xs uppercase tracking-widest text-muted-foreground">
          cross-venue skew (p99{venues.length > 1 ? `, ${venues.length} legs` : ''})
        </div>
        <div
          className={
            'mb-4 font-display text-3xl font-semibold tracking-tight ' +
            skewColor(crossVenueSkewNs ?? 0)
          }
        >
          {formatLatencyNs(crossVenueSkewNs ?? 0)}
        </div>

        <div className="overflow-x-auto">
          <table className="w-full text-left font-mono text-2xs">
            <thead className="text-muted-foreground">
              <tr className="border-b border-border">
                <th className="py-2 pr-4">venue</th>
                <th className="py-2 pr-4">symbol</th>
                <th className="py-2 pr-4 text-right">sent</th>
                <th className="py-2 pr-4 text-right">acked</th>
                <th className="py-2 pr-4 text-right">p50</th>
                <th className="py-2 pr-4 text-right">p99</th>
                <th className="py-2 pr-4 text-right">p99.9</th>
                <th className="py-2 pr-4 text-right">vs best</th>
              </tr>
            </thead>
            <tbody className="text-foreground">
              {venues.map((v) => (
                <tr key={v.venue_id} className="border-b border-border/40 last:border-b-0">
                  <td className="py-2 pr-4 text-accent">{v.venue_id}</td>
                  <td className="py-2 pr-4 text-muted-foreground">{v.symbol}</td>
                  <td className="py-2 pr-4 text-right">{v.sent_total.toLocaleString()}</td>
                  <td className="py-2 pr-4 text-right">{v.acked_total.toLocaleString()}</td>
                  <td className="py-2 pr-4 text-right">{formatLatencyNs(v.p50_ns)}</td>
                  <td
                    className={
                      'py-2 pr-4 text-right ' +
                      (v.p99_ns === maxP99 && maxP99 > minP99 ? 'text-signal-ask' : '')
                    }
                  >
                    {formatLatencyNs(v.p99_ns)}
                  </td>
                  <td className="py-2 pr-4 text-right">{formatLatencyNs(v.p999_ns)}</td>
                  <td className="py-2 pr-4 text-right text-muted-foreground">
                    {minP99 === Number.POSITIVE_INFINITY || v.p99_ns === minP99
                      ? 'best'
                      : `+${formatLatencyNs(v.p99_ns - minP99)}`}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </PanelBody>
    </Panel>
  )
}

function skewColor(ns: number): string {
  if (ns === 0)            return 'text-muted-foreground'
  if (ns < 10_000)         return 'text-signal-live'
  if (ns < 100_000)        return 'text-signal-info'
  return 'text-signal-ask'
}

function formatLatencyNs(ns: number): string {
  if (ns === 0) return '—'
  if (ns < 1_000) return `${ns}ns`
  if (ns < 1_000_000) return `${(ns / 1000).toFixed(1)}µs`
  if (ns < 1_000_000_000) return `${(ns / 1_000_000).toFixed(2)}ms`
  return `${(ns / 1_000_000_000).toFixed(2)}s`
}
