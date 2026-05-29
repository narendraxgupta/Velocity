/**
 * Ticker strip — the thin row of live platform metrics under the top nav.
 *
 * Mirrors the classic Bloomberg "BLOOMBERG INDICES" ticker but for our own
 * infrastructure: total bots online, aggregate RPS, p99 latency, active
 * submissions. Values are aggregated from the leaderboard stream (which is
 * itself driven by demo data when `NEXT_PUBLIC_DEMO_MODE=1`), giving the
 * top of the page a single, always-on heartbeat.
 */

'use client'

import { useEffect, useMemo, useState } from 'react'

import { useLeaderboardStream } from '@/lib/hooks/use-leaderboard-stream'
import { cn } from '@/lib/utils'

type TickerItem = {
  label: string
  value: string
  trend?: 'up' | 'down' | 'flat'
  tone?: 'live' | 'ask' | 'info' | 'warn' | 'neutral'
}

const toneClass: Record<NonNullable<TickerItem['tone']>, string> = {
  live: 'text-signal-live',
  ask: 'text-signal-ask',
  info: 'text-signal-info',
  warn: 'text-signal-warn',
  neutral: 'text-foreground',
}

export function TickerStrip() {
  const { rows, state } = useLeaderboardStream('global')
  const [tick, setTick] = useState(0)

  // Drive a 1Hz clock so the uptime field advances even when there are no
  // new leaderboard messages.
  useEffect(() => {
    const t = setInterval(() => setTick((n) => n + 1), 1_000)
    return () => clearInterval(t)
  }, [])

  const items = useMemo<TickerItem[]>(() => {
    const running = rows.filter((r) => r.status === 'running').length
    const totalRps = rows.reduce((acc, r) => acc + (r.throughputRps || 0), 0)
    const worstP99 = rows.reduce((acc, r) => Math.max(acc, r.p99Ns || 0), 0)
    const avgCorrect = rows.length
      ? rows.reduce((acc, r) => acc + r.correctness, 0) / rows.length
      : 0
    const uptime = formatUptime(tick)
    const stateTone: TickerItem['tone'] =
      state === 'open' ? 'live' : state === 'connecting' ? 'info' : 'warn'

    return [
      { label: 'STATUS',      value: state.toUpperCase(),                 tone: stateTone },
      { label: 'RUNNING',     value: `${running}/${rows.length || 0}`,    tone: 'info' },
      { label: 'AGG RPS',     value: formatRps(totalRps),                 tone: 'live', trend: 'up' },
      { label: 'WORST P99',   value: formatLatency(worstP99),             tone: worstP99 > 200_000 ? 'warn' : 'live' },
      { label: 'AVG CORRECT', value: avgCorrect ? `${avgCorrect.toFixed(2)}%` : '—', tone: 'neutral' },
      { label: 'SUBMISSIONS', value: rows.length.toString().padStart(2, '0'), tone: 'neutral' },
      { label: 'UPTIME',      value: uptime,                              tone: 'neutral' },
    ]
  }, [rows, state, tick])

  return (
    <div className="border-b border-border-subtle bg-surface-subtle">
      <div className="container flex h-8 items-center gap-6 overflow-x-auto py-0">
        {items.map((item) => (
          <div
            key={item.label}
            className="flex shrink-0 items-baseline gap-2 font-mono text-2xs uppercase tracking-wider"
          >
            <span className="text-muted-foreground">{item.label}</span>
            <span className={cn('font-semibold tabular-nums', toneClass[item.tone ?? 'neutral'])}>
              {item.value}
            </span>
            {item.trend && (
              <Trend dir={item.trend} className={cn(toneClass[item.tone ?? 'neutral'])} />
            )}
          </div>
        ))}
      </div>
    </div>
  )
}

function Trend({ dir, className }: { dir: 'up' | 'down' | 'flat'; className?: string }) {
  const symbol = dir === 'up' ? '▲' : dir === 'down' ? '▼' : '◆'
  return (
    <span aria-hidden className={cn('text-[10px]', className)}>
      {symbol}
    </span>
  )
}

function formatRps(n: number): string {
  if (n >= 1e6) return `${(n / 1e6).toFixed(2)}M`
  if (n >= 1e3) return `${(n / 1e3).toFixed(1)}k`
  return n.toString()
}

function formatLatency(ns: number): string {
  if (!ns) return '— µs'
  if (ns >= 1e6) return `${(ns / 1e6).toFixed(2)}ms`
  return `${(ns / 1_000).toFixed(1)}µs`
}

function formatUptime(seconds: number): string {
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = seconds % 60
  return `${pad(h)}:${pad(m)}:${pad(s)}`
}

function pad(n: number) { return n.toString().padStart(2, '0') }
