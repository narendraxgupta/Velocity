/**
 * /fleet — heatmap of the bot-worker registry.
 *
 * Each tile is one worker. Color encodes load (sustained RPS), with a red
 * ring around any worker whose error rate has crossed 1%. A stale (≥ 5s)
 * heartbeat collapses the tile to a muted ghost so operators can see
 * partitioned workers at a glance.
 */

'use client'

import { useMemo } from 'react'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { MetricCard } from '@/components/metric-card'
import { useFleet, type Worker } from '@/lib/hooks/use-fleet'
import { cn, formatLatencyNs, formatRelativeMs, formatRps } from '@/lib/utils'

export default function FleetPage() {
  const { workers, updatedAt, error } = useFleet()

  const totals = useMemo(() => {
    if (!workers) return null
    const sent = workers.reduce((acc, w) => acc + w.sentTotal, 0)
    const acked = workers.reduce((acc, w) => acc + w.ackedTotal, 0)
    const errored = workers.reduce((acc, w) => acc + w.erroredTotal, 0)
    const rps = workers.reduce((acc, w) => acc + w.currentRps, 0)
    const healthy = workers.filter((w) => w.healthy).length
    return { sent, acked, errored, rps, healthy }
  }, [workers])

  return (
    <div className="container space-y-6 py-8">
      <header className="flex items-end justify-between">
        <div>
          <span className="label-eyebrow">bot fleet · live</span>
          <h1 className="font-display text-2xl font-semibold tracking-tight">
            Fleet heatmap
          </h1>
          <p className="text-sm text-muted-foreground">
            Live worker registry from the bot-controller. Updates every 2 s.
            {error ? <span className="ml-2 text-signal-warn">· {error}</span> : null}
          </p>
        </div>
        <span className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
          {updatedAt ? `updated ${formatRelativeMs(Date.now() - updatedAt)}` : 'connecting…'}
        </span>
      </header>

      <section className="grid grid-cols-2 gap-3 md:grid-cols-5">
        <MetricCard label="Workers" value={workers?.length ?? '—'} hint={`${totals?.healthy ?? 0} healthy`} tone="live" />
        <MetricCard label="Aggregate RPS" value={totals ? formatRps(totals.rps) : '—'} unit="req/s" tone="ask" />
        <MetricCard label="Total Sent" value={totals?.sent.toLocaleString() ?? '—'} />
        <MetricCard label="Total Acked" value={totals?.acked.toLocaleString() ?? '—'} />
        <MetricCard
          label="Total Errored"
          value={totals?.errored.toLocaleString() ?? '—'}
          tone={totals && totals.errored > 0 ? 'warn' : 'neutral'}
        />
      </section>

      <Panel>
        <PanelHeader>
          <PanelTitle>Worker grid</PanelTitle>
          <PanelDescription>
            Each tile is one worker. Hue encodes sustained RPS; a red ring
            marks error rate &gt; 1 %; faded tiles are workers whose heartbeat
            has gone stale.
          </PanelDescription>
        </PanelHeader>
        <PanelBody>
          {!workers ? (
            <FleetSkeleton />
          ) : workers.length === 0 ? (
            <p className="text-sm text-muted-foreground">No workers registered yet.</p>
          ) : (
            <div className="grid grid-cols-2 gap-2 sm:grid-cols-4 md:grid-cols-6 lg:grid-cols-8">
              {workers.map((w) => (
                <WorkerTile key={w.workerId} worker={w} maxRps={Math.max(...workers.map((x) => x.currentRps))} />
              ))}
            </div>
          )}
        </PanelBody>
      </Panel>
    </div>
  )
}

/* -------------------------------------------------------------------------- */

function WorkerTile({ worker, maxRps }: { worker: Worker; maxRps: number }) {
  const intensity = maxRps > 0 ? worker.currentRps / maxRps : 0
  const ageMs = (Date.now() * 1_000_000 - worker.lastHeartbeatNs) / 1_000_000
  const stale = ageMs > 5_000

  // The tile uses an inline style so we can interpolate the color smoothly
  // along the green→amber→red axis without piling on Tailwind variants.
  const bg = stale
    ? 'rgba(40,42,46,0.55)'
    : `hsl(${152 - intensity * 60} 80% ${10 + intensity * 18}%)`

  return (
    <div
      className={cn(
        'group relative isolate flex flex-col gap-1 overflow-hidden rounded-md border px-3 py-2.5 text-foreground transition-colors',
        worker.errorRate > 0.01
          ? 'border-signal-warn/60 ring-1 ring-signal-warn/40'
          : 'border-border',
      )}
      style={{ backgroundColor: bg }}
      title={`${worker.hostname} · ${formatRps(worker.currentRps)} rps · err ${(worker.errorRate * 100).toFixed(2)}%`}
    >
      <div className="flex items-center justify-between">
        <span className="font-mono text-2xs uppercase tracking-wider text-foreground/80">
          {worker.workerId}
        </span>
        {stale && (
          <span className="rounded-sm bg-signal-warn/80 px-1 py-px font-mono text-[9px] uppercase tracking-widest text-background">
            stale
          </span>
        )}
      </div>
      <span className="font-display text-lg font-semibold leading-tight">
        {formatRps(worker.currentRps)}
      </span>
      <div className="font-mono text-[10px] uppercase tracking-wider text-foreground/60">
        cpu {worker.cpuPercent}% · {worker.activeBots} bots
      </div>
      <div className="font-mono text-[10px] uppercase tracking-wider text-foreground/50">
        err {(worker.errorRate * 100).toFixed(2)}%
      </div>
    </div>
  )
}

function FleetSkeleton() {
  return (
    <div className="grid grid-cols-2 gap-2 sm:grid-cols-4 md:grid-cols-6 lg:grid-cols-8">
      {Array.from({ length: 16 }).map((_, i) => (
        <div key={i} className="h-20 animate-pulse rounded-md bg-surface-elevated/60" />
      ))}
    </div>
  )
}
