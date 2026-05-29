/**
 * CliffCard — shows the throughput-cliff detector result.
 *
 * Rendered on the submission detail page when the post-hoc benchmark report
 * contains a `cliff` block (only set for cliff-finder profiles). The card
 * collapses to a single line when no cliff was observed in the run.
 */

'use client'

import { AlertTriangle, CheckCircle2 } from 'lucide-react'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { cn } from '@/lib/utils'

export type CliffResult = {
  detected:   boolean
  rps:        number
  lower_rps:  number
  upper_rps:  number
  confidence: number
  reason:     string
}

const REASON_LABEL: Record<string, string> = {
  throughput_shed:     'observed RPS dropped below 80% of offered',
  tail_latency_blowup: 'p99 exceeded 10× the early-ramp baseline',
}

export function CliffCard({ cliff }: { cliff: CliffResult }) {
  if (!cliff.detected) {
    return (
      <Panel>
        <PanelHeader>
          <PanelTitle>Throughput cliff</PanelTitle>
          <PanelDescription>
            Cliff-finder swept the full exponential staircase without ever
            seeing sustained RPS drop below 80 % of offered or p99 explode —
            this engine never broke during the run.
          </PanelDescription>
        </PanelHeader>
        <PanelBody>
          <div className="flex items-center gap-2 text-signal-live">
            <CheckCircle2 className="h-4 w-4" />
            <span className="font-mono text-sm">No cliff detected</span>
          </div>
        </PanelBody>
      </Panel>
    )
  }

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Throughput cliff</PanelTitle>
        <PanelDescription>
          The cliff-finder profile sweeps an exponential 5k → 1.28M RPS
          staircase and reports the first window where the engine shed
          throughput or its tail latency exploded.
        </PanelDescription>
      </PanelHeader>
      <PanelBody>
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-3">
          <Stat
            label="Cliff RPS"
            value={formatRps(cliff.rps)}
            hint={`CI: ${formatRps(cliff.lower_rps)} – ${formatRps(cliff.upper_rps)}`}
            tone="warn"
            icon={<AlertTriangle className="h-3.5 w-3.5" />}
          />
          <Stat
            label="Confidence"
            value={`${Math.round(cliff.confidence * 100)}%`}
            hint="Higher = longer breaking streak"
          />
          <Stat
            label="Reason"
            value={cliff.reason === 'throughput_shed' ? 'shed' : 'tail blowup'}
            hint={REASON_LABEL[cliff.reason] ?? cliff.reason}
          />
        </div>
      </PanelBody>
    </Panel>
  )
}

function Stat({
  label,
  value,
  hint,
  tone = 'neutral',
  icon,
}: {
  label: string
  value: string
  hint?: string
  tone?: 'neutral' | 'warn'
  icon?: React.ReactNode
}) {
  return (
    <div
      className={cn(
        'rounded-md border border-border bg-surface px-3 py-2',
        tone === 'warn' && 'border-signal-warn/40 bg-signal-warn/5',
      )}
    >
      <div className="flex items-center gap-1.5">
        {icon}
        <span className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
          {label}
        </span>
      </div>
      <div className={cn('mt-0.5 font-mono text-lg font-semibold tabular-nums',
        tone === 'warn' ? 'text-signal-warn' : 'text-foreground')}>
        {value}
      </div>
      {hint && (
        <p className="mt-0.5 text-2xs text-muted-foreground">{hint}</p>
      )}
    </div>
  )
}

function formatRps(v: number): string {
  if (!Number.isFinite(v) || v <= 0) return '—'
  if (v >= 1_000_000) return `${(v / 1_000_000).toFixed(2)}M`
  if (v >= 1_000)     return `${(v / 1_000).toFixed(1)}k`
  return v.toFixed(0)
}
