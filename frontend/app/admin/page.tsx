/**
 * /admin — operator console.
 *
 * Lets a platform operator:
 *   - pick a submission and a benchmark profile
 *   - start a benchmark (POST /v1/benchmarks)
 *   - cancel an in-flight benchmark (POST /v1/benchmarks/{id}/cancel)
 *   - see a small history of recent runs
 *
 * The right-hand "Run history" panel polls `localStorage` (we persist run
 * results client-side as a 30-entry ring) plus the leaderboard ZSET on the
 * backend.
 */

'use client'

import Link from 'next/link'
import { useCallback, useEffect, useState } from 'react'
import { toast } from 'sonner'

import { RoleGate } from '@/components/auth/role-gate'
import { ChaosPanel } from '@/components/chaos/chaos-panel'
import { MetricCard } from '@/components/metric-card'
import { PcapReplayPanel } from '@/components/pcap/replay-panel'
import { EmptyState } from '@/components/ui/empty-state'
import { Panel, PanelBody, PanelDescription, PanelFooter, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { isDemoMode } from '@/lib/demo-data'
import { cn, formatRelativeMs, middleTruncate } from '@/lib/utils'

const PROFILES = [
  { id: 'baseline',     name: 'Baseline',     hint: '50k rps · 30s hold · balanced mix' },
  { id: 'spike',        name: 'Spike',        hint: '200k rps · 15s hold · aggressive takers' },
  { id: 'fire-hose',    name: 'Fire-hose',    hint: '1M rps · 60s hold · stress sustained' },
  { id: 'adversarial',  name: 'Adversarial',  hint: '80k rps · 35s hold · spoofers + cancellers' },
  { id: 'cliff-finder', name: 'Cliff finder', hint: '5k → 1.28M rps · exponential staircase · pinpoints the breaking RPS with CI' },
  { id: 'cross-venue',  name: 'Cross-venue',  hint: '150k rps · SPOT/PERP/FUTURES · reports cross-venue p99 skew' },
] as const

type RunEntry = {
  benchmarkId: string
  submissionId: string
  profile: string
  startedAt: number
  status: 'starting' | 'running' | 'cancelled' | 'failed' | 'completed'
  message?: string
}

const STORAGE_KEY = 'velocity:admin:runs'

export default function AdminPage() {
  const [submissionId, setSubmissionId] = useState('')
  const [profile, setProfile] = useState<typeof PROFILES[number]['id']>('baseline')
  const [busy, setBusy] = useState(false)
  const [runs, setRuns] = useState<RunEntry[]>([])
  const [message, setMessage] = useState<string | null>(null)

  useEffect(() => {
    try {
      const raw = window.localStorage.getItem(STORAGE_KEY)
      if (raw) setRuns(JSON.parse(raw) as RunEntry[])
    } catch {}
  }, [])

  const persist = useCallback((next: RunEntry[] | ((prev: RunEntry[]) => RunEntry[])) => {
    setRuns((prev) => {
      const resolved = typeof next === 'function' ? next(prev) : next
      try { window.localStorage.setItem(STORAGE_KEY, JSON.stringify(resolved.slice(0, 30))) } catch {}
      return resolved
    })
  }, [])

  const start = useCallback(async () => {
    if (!submissionId) {
      setMessage('Submission id is required.')
      return
    }
    setBusy(true)
    setMessage(null)
    try {
      if (isDemoMode()) {
        const benchmarkId = `BM-DEMO-${Math.random().toString(16).slice(2, 10)}`
        persist((prev) => [{
          benchmarkId, submissionId, profile, startedAt: Date.now(),
          status: 'running', message: 'Demo mode — no backend.',
        }, ...prev])
        setMessage(`Started ${benchmarkId} (demo)`)
        setBusy(false)
        return
      }

      const res = await apiFetch(`/v1/benchmarks`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ submission_id: submissionId, profile }),
      })
      const body = await res.json()
      if (!res.ok) throw new Error(body.error ?? `HTTP ${res.status}`)

      const entry: RunEntry = {
        benchmarkId: body.benchmark_id,
        submissionId,
        profile,
        startedAt: Date.now(),
        status: 'running',
      }
      persist((prev) => [entry, ...prev])
      setMessage(`Started ${entry.benchmarkId}`)
      toast.success(`Benchmark started`, { description: entry.benchmarkId })
    } catch (e) {
      setMessage(`Failed: ${String(e)}`)
      toast.error('Failed to start benchmark', { description: String(e) })
    } finally {
      setBusy(false)
    }
  }, [submissionId, profile, persist])

  const cancel = useCallback(async (entry: RunEntry) => {
    setBusy(true)
    setMessage(null)
    try {
      if (!isDemoMode()) {
        const res = await apiFetch(`/v1/benchmarks/${encodeURIComponent(entry.benchmarkId)}/cancel`, {
          method: 'POST',
        })
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
      }
      persist((prev) =>
        prev.map((r) =>
          r.benchmarkId === entry.benchmarkId ? { ...r, status: 'cancelled' } : r,
        ),
      )
      setMessage(`Cancelled ${entry.benchmarkId}`)
      toast(`Benchmark cancelled`, { description: entry.benchmarkId })
    } catch (e) {
      setMessage(`Cancel failed: ${String(e)}`)
      toast.error('Failed to cancel benchmark', { description: String(e) })
    } finally {
      setBusy(false)
    }
  }, [persist])

  return (
    <div className="container space-y-6 py-8">
      <header className="flex items-end justify-between">
        <div>
          <span className="label-eyebrow">operator console</span>
          <h1 className="font-display text-2xl font-semibold tracking-tight">
            Admin
          </h1>
          <p className="text-sm text-muted-foreground">
            Start a benchmark for any submission, switch profile mid-flight by
            kicking off a new run, or cancel a stuck one.
          </p>
        </div>
        <div className="flex gap-2 text-2xs font-mono uppercase tracking-widest text-muted-foreground">
          <span>BENCHMARKS · {runs.length}</span>
        </div>
      </header>

      <section className="grid grid-cols-1 gap-3 lg:grid-cols-2">
        <Panel>
          <PanelHeader>
            <PanelTitle>Start a benchmark</PanelTitle>
            <PanelDescription>
              Pick a submission and a load profile. The bot controller is a
              singleton — at most one benchmark runs at a time.
            </PanelDescription>
          </PanelHeader>
          <PanelBody className="space-y-4">
            <div className="space-y-1.5">
              <label className="label-eyebrow" htmlFor="sub-id">Submission id</label>
              <input
                id="sub-id"
                value={submissionId}
                onChange={(e) => setSubmissionId(e.target.value.trim())}
                placeholder="01HQEAGIS001VPK7T7Q1QXKQ8N"
                className="w-full rounded border border-border bg-surface px-3 py-2 font-mono text-sm outline-none focus:border-accent focus:ring-1 focus:ring-accent"
              />
            </div>

            <fieldset className="space-y-2">
              <legend className="label-eyebrow">Profile</legend>
              <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
                {PROFILES.map((p) => (
                  <label
                    key={p.id}
                    className={cn(
                      'cursor-pointer rounded-md border px-3 py-2.5 transition-colors',
                      profile === p.id
                        ? 'border-accent bg-accent/10 text-foreground'
                        : 'border-border bg-surface text-muted-foreground hover:border-border-strong hover:text-foreground',
                    )}
                  >
                    <input
                      type="radio"
                      name="profile"
                      value={p.id}
                      checked={profile === p.id}
                      onChange={() => setProfile(p.id)}
                      className="sr-only"
                    />
                    <div className="font-display text-sm font-semibold">{p.name}</div>
                    <div className="font-mono text-2xs uppercase tracking-wider">{p.hint}</div>
                  </label>
                ))}
              </div>
            </fieldset>
          </PanelBody>
          <PanelFooter>
            <button
              onClick={start}
              disabled={busy || !submissionId}
              className="rounded-md bg-accent px-4 py-2 text-sm font-semibold text-background transition-opacity disabled:opacity-40"
            >
              {busy ? 'Working…' : 'Start benchmark'}
            </button>
            {message && (
              <span className="font-mono text-xs text-muted-foreground">{message}</span>
            )}
          </PanelFooter>
        </Panel>

        <Panel>
          <PanelHeader>
            <PanelTitle>Run history</PanelTitle>
            <PanelDescription>
              Locally remembered runs from this browser. Clear via dev tools’ {' '}
              <code className="font-mono">localStorage</code>.
            </PanelDescription>
          </PanelHeader>
          <PanelBody className="space-y-2">
            {runs.length === 0 ? (
              <EmptyState
                title="No runs yet"
                description="Start a benchmark on the left and it will appear here. History is stored in this browser only."
              />
            ) : (
              runs.map((r) => (
                <RunRow key={r.benchmarkId} run={r} onCancel={() => cancel(r)} />
              ))
            )}
          </PanelBody>
        </Panel>
      </section>

      <section className="grid grid-cols-1 gap-3 md:grid-cols-3">
        <MetricCard
          label="Latest profile"
          value={PROFILES.find((p) => p.id === profile)?.name ?? profile}
          hint={PROFILES.find((p) => p.id === profile)?.hint}
          tone="info"
        />
        <MetricCard
          label="Running"
          value={runs.filter((r) => r.status === 'running').length}
          tone="live"
        />
        <MetricCard
          label="Completed"
          value={runs.filter((r) => r.status === 'completed').length}
        />
      </section>

      {/* Chaos and pcap recording are operator+ only — the gateway will
          enforce this anyway, but hiding the controls keeps the surface
          honest for submitter accounts. */}
      <RoleGate cap="chaos:inject">
        <ChaosPanel />
      </RoleGate>

      <RoleGate cap="pcaps:record">
        <PcapReplayPanel />
      </RoleGate>
    </div>
  )
}

/* -------------------------------------------------------------------------- */

function RunRow({ run, onCancel }: { run: RunEntry; onCancel: () => void }) {
  const cls: Record<RunEntry['status'], string> = {
    starting:  'text-signal-info border-signal-info/40',
    running:   'text-signal-live border-signal-live/40',
    completed: 'text-accent border-accent/40',
    cancelled: 'text-muted-foreground border-border-subtle',
    failed:    'text-signal-warn border-signal-warn/40',
  }
  return (
    <div className="flex items-center justify-between gap-3 rounded-md border border-border bg-surface px-3 py-2">
      <div className="min-w-0 flex-1">
        <div className="flex items-center gap-2">
          <span className={cn('rounded border px-1.5 py-0.5 font-mono text-2xs font-semibold uppercase tracking-widest', cls[run.status])}>
            {run.status}
          </span>
          <span className="font-mono text-xs">{middleTruncate(run.benchmarkId, 18)}</span>
        </div>
        <div className="mt-0.5 font-mono text-2xs uppercase tracking-wider text-muted-foreground">
          {run.profile} · {middleTruncate(run.submissionId, 18)} · {formatRelativeMs(Date.now() - run.startedAt)}
        </div>
      </div>
      <div className="flex items-center gap-2">
        <Link
          href={`/submissions/${encodeURIComponent(run.benchmarkId)}`}
          className="text-xs font-medium text-accent hover:underline"
        >
          watch →
        </Link>
        {run.status === 'running' && (
          <button
            onClick={onCancel}
            className="rounded border border-signal-warn/40 bg-signal-warn/10 px-2 py-1 font-mono text-2xs font-semibold uppercase tracking-widest text-signal-warn hover:bg-signal-warn/20"
          >
            cancel
          </button>
        )}
      </div>
    </div>
  )
}
