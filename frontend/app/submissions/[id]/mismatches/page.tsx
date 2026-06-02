/**
 * /submissions/[id]/mismatches — correctness drilldown.
 *
 * Renders the taxonomy of violations the correctness validator detected
 * for this submission, plus a sample table of the most recent offending
 * orders (reference book state vs the submission's reported book state).
 *
 * Data:
 *   GET /v1/submissions/:id/mismatches
 *      => { taxonomy: {...}, samples: [{ correlation_id, kind, reference[], reported, ts_ns }] }
 *
 * The taxonomy counts come from the per-submission Redis hash maintained
 * by scoring-service. The sample list comes from a small ring buffer the
 * validator pushes into Redis LIST `mismatches:<id>` as it observes
 * violations (see correctness-validator/src/validator.cpp::push_mismatch).
 * Each sample is `{ ts_ns, correlation_id, kind, reported:{quantity,price,
 * outcome}, reference:[{price,quantity}, …] }` — the `reference` array is the
 * fills the gold book produced; `reported` is what the submission claimed.
 */

'use client'

import Link from 'next/link'
import { useParams } from 'next/navigation'
import { useEffect, useMemo, useState } from 'react'

import { MetricCard } from '@/components/metric-card'
import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { isDemoMode } from '@/lib/demo-data'
import { cn, middleTruncate } from '@/lib/utils'

type Taxonomy = {
  priority_violations: number
  price_violations: number
  phantom_fills: number
  missing_fills: number
  expected_fills: number
  actual_fills: number
  correct_fills: number
}

type Fill = { price?: number; quantity?: number }

type Sample = {
  /** Validator emits a numeric correlation id; older/demo data used order_id. */
  correlation_id?: number | string
  order_id?: string
  side?: 'BUY' | 'SELL'
  kind: 'price' | 'priority' | 'phantom' | 'missing' | 'self-cross' | string
  /** Gold-book fills (array). Demo fixtures use the legacy `ref` object. */
  reference?: Fill[]
  ref?: { price?: number; quantity?: number; book?: string }
  reported?: { price?: number; quantity?: number; outcome?: number; book?: string }
  ts_ns?: number
}

export default function MismatchesPage() {
  const { id } = useParams<{ id: string }>()
  const [data, setData] = useState<{ taxonomy: Taxonomy; samples: Sample[] } | null>(null)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    let cancelled = false
    const load = async () => {
      if (isDemoMode()) {
        setData({ taxonomy: demoTax, samples: demoSamples })
        return
      }
      try {
        const res = await apiFetch(
          `/v1/submissions/${encodeURIComponent(id)}/mismatches?n=100`,
          { cache: 'no-store' },
        )
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        const json = await res.json()
        if (!cancelled) setData(json)
      } catch (e) {
        if (!cancelled) setError(String(e))
      }
    }
    load()
    const t = setInterval(load, 4000)
    return () => {
      cancelled = true
      clearInterval(t)
    }
  }, [id])

  const accuracy = useMemo(() => {
    if (!data) return null
    // Keep this in sync with the validator's formula (docs/scoring.md §4):
    // accuracy = 100 * correct_fills / max(1, expected_fills). Using
    // expected (not max(expected, actual)) avoids letting a submission
    // hide missing fills by over-reporting phantom ones.
    const denom = Math.max(1, data.taxonomy.expected_fills)
    return (100 * data.taxonomy.correct_fills) / denom
  }, [data])

  return (
    <div className="container space-y-6 py-8">
      <header>
        <span className="label-eyebrow">submission · correctness drilldown</span>
        <h1 className="font-display text-2xl font-semibold tracking-tight">
          {middleTruncate(id, 28)} — Mismatches
        </h1>
        <p className="text-sm text-muted-foreground">
          Per-order reconciliation against the reference price-time priority orderbook. {' '}
          <Link href={`/submissions/${id}`} className="text-accent hover:underline">
            ← back to run
          </Link>
          {error ? <span className="ml-2 text-signal-warn">· {error}</span> : null}
        </p>
      </header>

      <section className="grid grid-cols-2 gap-3 md:grid-cols-4">
        <MetricCard label="Accuracy" value={accuracy != null ? accuracy.toFixed(2) : '—'} unit="%" tone="live" />
        <MetricCard label="Price violations"    value={data?.taxonomy.price_violations ?? '—'}    tone="warn" />
        <MetricCard label="Priority violations" value={data?.taxonomy.priority_violations ?? '—'} tone="warn" />
        <MetricCard label="Phantom fills"       value={data?.taxonomy.phantom_fills ?? '—'}        tone="warn" />
      </section>

      <Panel>
        <PanelHeader>
          <PanelTitle>Sample violations</PanelTitle>
          <PanelDescription>
            The most recent 100 offending orders. Each row shows the reference
            book state at match time alongside the submission&apos;s reported fill.
          </PanelDescription>
        </PanelHeader>
        <PanelBody className="overflow-x-auto">
          {!data ? (
            <p className="text-sm text-muted-foreground">Loading…</p>
          ) : data.samples.length === 0 ? (
            <p className="text-sm text-muted-foreground">
              No sample violations recorded. (Live capture is pushed by the validator
              to <code className="font-mono">mismatches:&lt;id&gt;</code> as it observes them.)
            </p>
          ) : (
            <table className="min-w-full text-sm">
              <thead>
                <tr className="text-left text-2xs uppercase tracking-wider text-muted-foreground">
                  <th className="py-2 pr-4">Order</th>
                  <th className="py-2 pr-4">Kind</th>
                  <th className="py-2 pr-4">Side</th>
                  <th className="py-2 pr-4">Reference</th>
                  <th className="py-2 pr-4">Reported</th>
                  <th className="py-2 pr-4">When</th>
                </tr>
              </thead>
              <tbody>
                {data.samples.map((s, i) => (
                  <tr key={i} className="border-t border-border-subtle font-mono text-xs">
                    <td className="py-1.5 pr-4">{formatOrderId(s)}</td>
                    <td className="py-1.5 pr-4">
                      <KindBadge kind={s.kind} />
                    </td>
                    <td className="py-1.5 pr-4">{s.side ?? '—'}</td>
                    <td className="py-1.5 pr-4">{formatReference(s)}</td>
                    <td className="py-1.5 pr-4">{formatPair(s.reported)}</td>
                    <td className="py-1.5 pr-4">{s.ts_ns ? new Date(s.ts_ns / 1_000_000).toISOString().split('T')[1] : '—'}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </PanelBody>
      </Panel>
    </div>
  )
}

/* -------------------------------------------------------------------------- */

function KindBadge({ kind }: { kind: string }) {
  const cls: Record<string, string> = {
    price: 'text-signal-warn border-signal-warn/40 bg-signal-warn/10',
    priority: 'text-signal-ask border-signal-ask/40 bg-signal-ask/10',
    phantom: 'text-signal-warn border-signal-warn/40 bg-signal-warn/10',
    missing: 'text-signal-info border-signal-info/40 bg-signal-info/10',
    'self-cross': 'text-accent border-accent/40 bg-accent/10',
  }
  return (
    <span
      className={cn(
        'rounded border px-1.5 py-0.5 text-2xs font-semibold uppercase tracking-widest',
        cls[kind] ?? 'border-border-subtle text-muted-foreground',
      )}
    >
      {kind}
    </span>
  )
}

function formatPair(p?: { price?: number; quantity?: number; book?: string }) {
  if (!p) return '—'
  if (p.book) return p.book
  if (p.price != null && p.quantity != null) return `${p.quantity} @ ${p.price}`
  return '—'
}

function formatOrderId(s: Sample): string {
  if (s.order_id) return middleTruncate(s.order_id, 16)
  if (s.correlation_id != null && s.correlation_id !== '') {
    return middleTruncate(String(s.correlation_id), 16)
  }
  return '—'
}

/**
 * The validator pushes the gold-book fills as an array of {price, quantity};
 * older/demo data used a single `ref` object. Render whichever is present.
 */
function formatReference(s: Sample): string {
  if (Array.isArray(s.reference)) {
    if (s.reference.length === 0) return '(no fill)'
    return s.reference
      .filter((f) => f.price != null && f.quantity != null)
      .map((f) => `${f.quantity} @ ${f.price}`)
      .join(', ') || '—'
  }
  return formatPair(s.ref)
}

/* Demo fixtures ----------------------------------------------------------- */

const demoTax: Taxonomy = {
  priority_violations: 14,
  price_violations: 6,
  phantom_fills: 2,
  missing_fills: 5,
  expected_fills: 12_345,
  actual_fills: 12_341,
  correct_fills: 12_318,
}

const demoSamples: Sample[] = [
  {
    order_id: '01HQDEMO001',
    side: 'BUY',
    kind: 'price',
    ref:      { price: 100_0500, quantity: 10, book: '100.0500 / 100.0510' },
    reported: { price: 100_0510, quantity: 10, book: '100.0500 / 100.0510' },
    ts_ns: Date.now() * 1_000_000,
  },
  {
    order_id: '01HQDEMO002',
    side: 'SELL',
    kind: 'priority',
    ref:      { price: 99_9500, quantity: 25, book: 'maker @ T-12µs' },
    reported: { price: 99_9500, quantity: 25, book: 'maker @ T-4µs' },
    ts_ns: Date.now() * 1_000_000 - 12_000,
  },
  {
    order_id: '01HQDEMO003',
    side: 'BUY',
    kind: 'phantom',
    ref:      { book: '(no matching maker)' },
    reported: { price: 100_0500, quantity: 5 },
    ts_ns: Date.now() * 1_000_000 - 28_000,
  },
  {
    order_id: '01HQDEMO004',
    side: 'SELL',
    kind: 'missing',
    ref:      { price: 100_0500, quantity: 8 },
    reported: { book: '(no fill reported)' },
    ts_ns: Date.now() * 1_000_000 - 41_000,
  },
]
