/**
 * PhasePill — small status badge for the BenchmarkSnapshot lifecycle.
 *
 * Five canonical states. Pulse on the LIVE ones; static on terminal ones.
 */

import { cn } from '@/lib/utils'
import type { BenchmarkPhase } from '@/lib/hooks/use-benchmark-stream'

const STYLES: Record<BenchmarkPhase, { label: string; cls: string; live: boolean }> = {
  unspecified: { label: '—',          cls: 'text-muted-foreground border-border-subtle bg-surface', live: false },
  queued:      { label: 'Queued',     cls: 'text-signal-info border-signal-info/40 bg-signal-info/10', live: true },
  ramping:     { label: 'Ramping',    cls: 'text-signal-ask border-signal-ask/40 bg-signal-ask/10',   live: true },
  holding:     { label: 'Holding',    cls: 'text-signal-live border-signal-live/40 bg-signal-live/10', live: true },
  draining:    { label: 'Draining',   cls: 'text-signal-warn border-signal-warn/40 bg-signal-warn/10', live: true },
  complete:    { label: 'Complete',   cls: 'text-accent border-accent/40 bg-accent/10',                live: false },
  cancelled:   { label: 'Cancelled',  cls: 'text-muted-foreground border-border-subtle bg-surface',   live: false },
  failed:      { label: 'Failed',     cls: 'text-signal-warn border-signal-warn/40 bg-signal-warn/10', live: false },
}

export function PhasePill({ phase }: { phase: BenchmarkPhase }) {
  const s = STYLES[phase]
  return (
    <span
      className={cn(
        'inline-flex items-center gap-1.5 rounded border px-2 py-1 font-mono text-2xs font-semibold uppercase tracking-widest',
        s.cls,
      )}
    >
      {s.live && <span className="live-dot" aria-hidden />}
      {s.label}
    </span>
  )
}
