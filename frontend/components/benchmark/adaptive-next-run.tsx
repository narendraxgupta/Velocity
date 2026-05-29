/**
 * AdaptiveNextRun — surfaces the anomaly-detector's "what to run next"
 * verdict on the submission detail page.
 *
 * Behaviour
 * ---------
 * 1. On mount, GET /v1/submissions/{id}/adaptive-profile. 404 → hide
 *    the panel until the user explicitly asks for a suggestion.
 * 2. "Suggest profile" button POSTs to the same path with the latest
 *    benchmark report we have in scope. Server returns a verdict
 *    with profile_name + reason.
 * 3. "Run with this profile" calls POST /v1/benchmarks with the
 *    suggested profile against this submission.
 */

'use client'

import { useEffect, useState } from 'react'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { cn } from '@/lib/utils'

interface AdaptivePick {
  submission_id: string
  profile_name:  string
  reason:        string
  inputs:        Record<string, unknown>
}

const PROFILE_DESCRIPTION: Record<string, string> = {
  'baseline':     'Steady 50k req/s for 30s — the canonical run.',
  'cliff-finder': 'Exponential RPS staircase to pin the cliff with confidence.',
  'fire-hose':    '1M req/s — pushes the throughput ceiling.',
  'adversarial':  'Spoofer + canceller heavy — exposes matcher edge-cases.',
  'soak':         'Lower RPS, longer duration — surfaces correctness drift.',
  'cross-venue':  'SPOT + PERP + FUTURES — exposes cross-venue latency skew.',
  'adaptive-rl':  'PPO-trained adaptive bots — non-stationary load shape.',
}

export function AdaptiveNextRun({
  submissionId,
  report,
}: {
  submissionId: string
  report?: unknown
}) {
  const [pick,     setPick]   = useState<AdaptivePick | null>(null)
  const [loading,  setLoading] = useState(false)
  const [running,  setRunning] = useState(false)
  const [err,      setErr]     = useState<string | null>(null)

  // Initial check — has anything been cached for this submission?
  useEffect(() => {
    if (!submissionId) return undefined
    setPick(null)
    setErr(null)
    let cancelled = false
    apiFetch(`/v1/submissions/${encodeURIComponent(submissionId)}/adaptive-profile`)
      .then(async (r) => {
        if (cancelled || !r.ok) return
        const body = (await r.json()) as AdaptivePick
        setPick(body)
      })
      .catch(() => { /* ignore */ })
    return () => { cancelled = true }
  }, [submissionId])

  const suggest = async () => {
    setLoading(true)
    setErr(null)
    try {
      const r = await apiFetch(
        `/v1/submissions/${encodeURIComponent(submissionId)}/adaptive-profile`,
        {
          method:  'POST',
          headers: { 'Content-Type': 'application/json' },
          body:    JSON.stringify({ report }),
        },
      )
      if (!r.ok) throw new Error(`gateway ${r.status}`)
      const body = (await r.json()) as AdaptivePick
      setPick(body)
    } catch (e) {
      setErr((e as Error).message)
    } finally {
      setLoading(false)
    }
  }

  const runIt = async () => {
    if (!pick) return
    setRunning(true)
    setErr(null)
    try {
      const r = await apiFetch(`/v1/benchmarks`, {
        method:  'POST',
        headers: { 'Content-Type': 'application/json' },
        body:    JSON.stringify({
          submission_id: submissionId,
          profile:       pick.profile_name,
        }),
      })
      if (!r.ok) throw new Error(`gateway ${r.status}`)
    } catch (e) {
      setErr((e as Error).message)
    } finally {
      setRunning(false)
    }
  }

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Adaptive next run</PanelTitle>
        <PanelDescription>
          The detector picks the benchmark profile most likely to expose this
          submission&apos;s next weak spot, based on cliff / regression / health /
          execution-quality data from prior runs.
        </PanelDescription>
      </PanelHeader>
      <PanelBody className="space-y-3">
        {pick ? (
          <>
            <div className="flex flex-wrap items-baseline gap-3">
              <span className="rounded-sm border border-accent/40 bg-accent/10 px-2 py-1 font-mono text-2xs font-semibold uppercase tracking-wider text-accent">
                {pick.profile_name}
              </span>
              <span className="text-sm text-muted-foreground">
                {PROFILE_DESCRIPTION[pick.profile_name] ?? '—'}
              </span>
            </div>
            <p className="text-sm">{pick.reason}</p>
          </>
        ) : (
          <p className="text-sm text-muted-foreground">
            No suggestion yet — generate one from the report so far.
          </p>
        )}

        {err && <p className="text-sm text-signal-warn">{err}</p>}

        <div className="flex gap-2">
          <button
            type="button"
            onClick={suggest}
            disabled={loading}
            className={cn(
              'rounded-md border border-border-subtle px-3 py-1.5 text-sm font-medium hover:bg-surface-elevated',
              loading && 'opacity-60',
            )}
          >
            {loading ? 'Computing…' : pick ? 'Recompute' : 'Suggest profile'}
          </button>
          {pick && (
            <button
              type="button"
              onClick={runIt}
              disabled={running}
              className={cn(
                'rounded-md bg-accent px-3 py-1.5 text-sm font-medium text-accent-foreground hover:opacity-90',
                running && 'opacity-60',
              )}
            >
              {running ? 'Starting…' : `Run with ${pick.profile_name}`}
            </button>
          )}
        </div>
      </PanelBody>
    </Panel>
  )
}
