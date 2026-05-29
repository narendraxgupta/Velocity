/**
 * CritiquePanel — submission detail "Review" tab content.
 *
 * Lifecycle
 * ---------
 *   1. Mount: GET /v1/submissions/{id}/critique. If present, render.
 *   2. Empty: show a "Generate critique" button.
 *   3. User triggers: POST /v1/critiques with the submission context
 *      (most fields autopopulated from the live benchmark snapshot we
 *      pass in via props). Server returns a critique_id + status=running.
 *   4. Poll: GET /v1/critiques/{critique_id} every 2s until the record
 *      materialises. We stop polling after 90s with a "took too long"
 *      banner — the user can retry.
 *
 * No client-side LLM calls. All requests go through the API gateway,
 * which proxies to critique-service, which talks to local Ollama.
 */

'use client'

import { useCallback, useEffect, useRef, useState } from 'react'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { cn } from '@/lib/utils'

type Priority = 'high' | 'medium' | 'low'

interface Suggestion {
  priority: Priority
  title: string
  rationale: string
}

interface Critique {
  summary: string
  strengths: string[]
  risks: string[]
  suggestions: Suggestion[]
  microstructure: string[]
  latency: string[]
  correctness: string[]
}

// NB: deliberately NOT named `Record` — that would shadow the global
// `Record<K,V>` utility type and break the `Record<Priority, string>`
// mapping used by SuggestionList below. TypeScript surfaces this as
// "Type 'Record' is not generic" because the local interface wins
// name resolution.
interface CritiqueRecord {
  critique_id: string
  submission_id: string
  model: string
  created_at_ms: number
  latency_ms: number
  prompt_tokens?: number
  eval_tokens?: number
  critique: Critique
}

export interface CritiqueContext {
  submissionId: string
  language: string
  kind: string
  source: string
  score: number
  throughputRps: number
  p50LatencyNs: number
  p99LatencyNs: number
  p999LatencyNs: number
  correctnessRatio: number
  orderRejectRate: number
  slippageBps?: number
  isBps?: number
  reversionBps?: number
  kernelUserspaceSkewNs?: number
}

type State =
  | { phase: 'idle' }
  | { phase: 'loading' }
  | { phase: 'empty' }
  | { phase: 'running'; critiqueId: string; elapsed: number }
  | { phase: 'ready'; record: CritiqueRecord }
  | { phase: 'error'; message: string }

export function CritiquePanel({ context }: { context: CritiqueContext | null }) {
  const [state, setState] = useState<State>({ phase: 'idle' })
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null)

  const clearPoll = useCallback(() => {
    if (pollRef.current) {
      clearInterval(pollRef.current)
      pollRef.current = null
    }
  }, [])

  // Initial fetch: do we already have a critique?
  useEffect(() => {
    if (!context?.submissionId) return undefined
    let cancelled = false
    setState({ phase: 'loading' })
    apiFetch(`/v1/submissions/${encodeURIComponent(context.submissionId)}/critique`)
      .then(async (r) => {
        if (cancelled) return
        if (r.status === 404) {
          setState({ phase: 'empty' })
          return
        }
        if (!r.ok) throw new Error(`gateway ${r.status}`)
        const rec = (await r.json()) as CritiqueRecord
        setState({ phase: 'ready', record: rec })
      })
      .catch((err: Error) => {
        if (!cancelled) setState({ phase: 'error', message: err.message })
      })
    return () => {
      cancelled = true
    }
  }, [context?.submissionId])

  // Cleanup any timers when we unmount.
  useEffect(() => () => clearPoll(), [clearPoll])

  const generate = useCallback(async () => {
    if (!context) return
    setState({ phase: 'running', critiqueId: '', elapsed: 0 })
    try {
      const body = {
        submission_id: context.submissionId,
        language: context.language,
        kind: context.kind,
        source: context.source,
        score: context.score,
        throughput_rps: context.throughputRps,
        p50_latency_ns: context.p50LatencyNs,
        p99_latency_ns: context.p99LatencyNs,
        p999_latency_ns: context.p999LatencyNs,
        correctness_ratio: context.correctnessRatio,
        order_reject_rate: context.orderRejectRate,
        slippage_bps: context.slippageBps,
        is_bps: context.isBps,
        reversion_bps: context.reversionBps,
        kernel_userspace_skew_ns: context.kernelUserspaceSkewNs,
      }
      const r = await apiFetch(`/v1/critiques`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      })
      if (!r.ok) throw new Error(`gateway ${r.status}`)
      const { critique_id: cid } = (await r.json()) as { critique_id: string }
      const startedAt = Date.now()
      setState({ phase: 'running', critiqueId: cid, elapsed: 0 })

      clearPoll()
      pollRef.current = setInterval(async () => {
        const elapsed = Math.round((Date.now() - startedAt) / 1000)
        if (elapsed > 90) {
          clearPoll()
          setState({
            phase: 'error',
            message: 'Critique took longer than 90s — please retry.',
          })
          return
        }
        try {
          const r = await apiFetch(`/v1/critiques/${cid}`)
          if (r.status === 202) {
            setState({ phase: 'running', critiqueId: cid, elapsed })
            return
          }
          if (!r.ok) throw new Error(`gateway ${r.status}`)
          const rec = (await r.json()) as CritiqueRecord
          clearPoll()
          setState({ phase: 'ready', record: rec })
        } catch (err) {
          // Single poll failures aren't fatal; we keep polling until
          // the 90s budget elapses.
          setState({ phase: 'running', critiqueId: cid, elapsed })
        }
      }, 2000)
    } catch (err) {
      const e = err as Error
      setState({ phase: 'error', message: e.message })
    }
  }, [clearPoll, context])

  if (!context) {
    return (
      <Panel>
        <PanelHeader>
          <PanelTitle>Strategy review</PanelTitle>
          <PanelDescription>Waiting for the first benchmark snapshot…</PanelDescription>
        </PanelHeader>
      </Panel>
    )
  }

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Strategy review</PanelTitle>
        <PanelDescription>
          A local LLM critiques your strategy against this run&apos;s benchmark report —
          source stays on-cluster. Outputs are deterministic per submission ID.
        </PanelDescription>
      </PanelHeader>
      <PanelBody>
        {state.phase === 'idle' || state.phase === 'loading' ? (
          <p className="text-sm text-muted-foreground">Checking for an existing critique…</p>
        ) : null}

        {state.phase === 'empty' && (
          <div className="space-y-3">
            <p className="text-sm text-muted-foreground">
              No critique has been generated for this submission yet.
            </p>
            <button
              type="button"
              onClick={generate}
              className="rounded-md bg-accent px-3 py-1.5 text-sm font-medium text-accent-foreground hover:opacity-90"
            >
              Generate critique
            </button>
          </div>
        )}

        {state.phase === 'running' && (
          <div className="space-y-2">
            <p className="text-sm text-muted-foreground">
              Local LLM running… elapsed{' '}
              <span className="font-mono text-foreground">{state.elapsed}s</span>
            </p>
            <div className="h-1 w-full overflow-hidden rounded bg-surface-elevated">
              <div className="h-full w-1/3 animate-pulse rounded bg-accent" />
            </div>
          </div>
        )}

        {state.phase === 'error' && (
          <div className="space-y-3">
            <p className="text-sm text-signal-warn">{state.message}</p>
            <button
              type="button"
              onClick={generate}
              className="rounded-md border border-border-subtle px-3 py-1.5 text-sm font-medium hover:bg-surface-elevated"
            >
              Retry
            </button>
          </div>
        )}

        {state.phase === 'ready' && <CritiqueReadOnly record={state.record} onRegenerate={generate} />}
      </PanelBody>
    </Panel>
  )
}

function CritiqueReadOnly({
  record,
  onRegenerate,
}: {
  record: CritiqueRecord
  onRegenerate: () => void
}) {
  const c = record.critique
  return (
    <div className="space-y-5">
      <header className="flex flex-wrap items-baseline justify-between gap-2 border-b border-border-subtle pb-3">
        <p className="font-mono text-2xs uppercase tracking-wider text-muted-foreground">
          {record.model} · {formatLatency(record.latency_ms)} · {record.eval_tokens ?? 0} tokens
        </p>
        <button
          type="button"
          onClick={onRegenerate}
          className="text-sm font-medium text-accent hover:underline"
        >
          Regenerate
        </button>
      </header>

      <section>
        <h4 className="label-eyebrow">Summary</h4>
        <p className="mt-1 text-sm">{c.summary}</p>
      </section>

      <SuggestionList items={c.suggestions} />

      <div className="grid gap-4 sm:grid-cols-2">
        <BulletList title="Strengths" items={c.strengths} tone="live" />
        <BulletList title="Risks" items={c.risks} tone="warn" />
      </div>
      <div className="grid gap-4 sm:grid-cols-3">
        <BulletList title="Microstructure" items={c.microstructure} />
        <BulletList title="Latency" items={c.latency} />
        <BulletList title="Correctness" items={c.correctness} />
      </div>
    </div>
  )
}

function SuggestionList({ items }: { items: Suggestion[] }) {
  if (items.length === 0) return null
  const priorityClass: Record<Priority, string> = {
    high: 'bg-signal-warn/15 text-signal-warn',
    medium: 'bg-signal-info/15 text-signal-info',
    low: 'bg-surface-elevated text-foreground',
  }
  return (
    <section>
      <h4 className="label-eyebrow">Suggestions</h4>
      <ol className="mt-2 space-y-3">
        {items.map((s, i) => (
          <li key={i} className="rounded-md border border-border-subtle bg-surface-subtle p-3">
            <div className="flex items-start gap-2">
              <span
                className={cn(
                  'rounded px-1.5 py-0.5 font-mono text-2xs uppercase tracking-wider',
                  priorityClass[s.priority],
                )}
              >
                {s.priority}
              </span>
              <p className="text-sm font-medium">{s.title}</p>
            </div>
            <p className="mt-1 text-sm text-muted-foreground">{s.rationale}</p>
          </li>
        ))}
      </ol>
    </section>
  )
}

function BulletList({
  title,
  items,
  tone,
}: {
  title: string
  items: string[]
  tone?: 'live' | 'warn'
}) {
  if (items.length === 0) return null
  const toneClass =
    tone === 'live' ? 'border-signal-live/40' : tone === 'warn' ? 'border-signal-warn/40' : ''
  return (
    <div className={cn('rounded-md border border-border-subtle p-3', toneClass)}>
      <h4 className="label-eyebrow">{title}</h4>
      <ul className="mt-2 space-y-1.5 text-sm">
        {items.map((it, i) => (
          <li key={i} className="leading-snug">
            • {it}
          </li>
        ))}
      </ul>
    </div>
  )
}

function formatLatency(ms: number): string {
  if (!ms) return '—'
  if (ms < 1000) return `${ms}ms`
  return `${(ms / 1000).toFixed(1)}s`
}
