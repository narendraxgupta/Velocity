/**
 * MarketdataTicker — live mid-price strip backed by the gateway SSE feed.
 *
 * Wire format
 * -----------
 * The gateway streams `text/event-stream` events of type `snapshot` with
 * JSON payload `{ ts_ms, symbols: { [sym]: { mid, mid_units, price_scale } } }`,
 * emitted every 100ms by the marketdata-generator service. Heartbeats
 * (`: heartbeat`) keep proxies from collapsing the connection.
 *
 * Behaviour
 * ---------
 * - Connects on mount, reconnects with exponential back-off on stream
 *   errors (cap 5s — these are observability frames, not critical path).
 * - Holds the last value per symbol so reconnects don't blank the UI.
 * - Computes per-symbol direction (up / down / flat) against the previous
 *   tick to drive the colour and arrow glyph.
 * - In demo mode (NEXT_PUBLIC_DEMO_MODE=1) we still attempt the SSE in
 *   case the API gateway is wired locally; if it fails twice we fall
 *   back to a synthetic OU process on the client so the page is never
 *   blank during demos.
 */

'use client'

import { useEffect, useMemo, useRef, useState } from 'react'

import { cn } from '@/lib/utils'

type SymbolTick = {
  symbol: string
  mid: number
  midUnits: number
  priceScale: number
}

type Direction = 'up' | 'down' | 'flat'

const API_BASE = process.env.NEXT_PUBLIC_API_GATEWAY_URL || 'http://localhost:8080'
const DEMO = process.env.NEXT_PUBLIC_DEMO_MODE === '1'

export function MarketdataTicker() {
  const [ticks, setTicks] = useState<Record<string, SymbolTick>>({})
  const [directions, setDirections] = useState<Record<string, Direction>>({})
  const prevMids = useRef<Record<string, number>>({})
  const failureCount = useRef(0)
  const fallbackTimer = useRef<ReturnType<typeof setInterval> | null>(null)

  useEffect(() => {
    let cancelled = false
    let es: EventSource | null = null
    let retryTimer: ReturnType<typeof setTimeout> | null = null
    let retryMs = 250

    const startFallback = () => {
      if (fallbackTimer.current) return
      // Synthetic OU walk so the strip is never blank in offline demos.
      const synth: Record<string, SymbolTick> = {
        'SPOT/USDT': { symbol: 'SPOT/USDT', mid: 100.0,  midUnits: 100_000_000, priceScale: 1_000_000 },
        'BTC-PERP':  { symbol: 'BTC-PERP',  mid: 100.05, midUnits: 100_050_000, priceScale: 1_000_000 },
        'BTC-DEC25': { symbol: 'BTC-DEC25', mid: 99.95,  midUnits:  99_950_000, priceScale: 1_000_000 },
      }
      setTicks(synth)
      fallbackTimer.current = setInterval(() => {
        setTicks((prev) => {
          const next: Record<string, SymbolTick> = { ...prev }
          for (const sym of Object.keys(next)) {
            const t = next[sym]!
            // Small zero-mean drift: OU step with θ=0.05, σ=0.02.
            const noise = (Math.random() * 2 - 1) * 0.02
            const newMid = t.mid + 0.05 * (t.mid - t.mid) + noise
            next[sym] = { ...t, mid: newMid, midUnits: Math.round(newMid * t.priceScale) }
          }
          return next
        })
      }, 250)
    }

    const stopFallback = () => {
      if (fallbackTimer.current) {
        clearInterval(fallbackTimer.current)
        fallbackTimer.current = null
      }
    }

    const open = () => {
      if (cancelled) return
      try {
        es = new EventSource(`${API_BASE}/v1/marketdata/stream`)
      } catch {
        failureCount.current += 1
        if (DEMO && failureCount.current >= 2) startFallback()
        return
      }
      es.addEventListener('snapshot', (ev: MessageEvent) => {
        try {
          const body = JSON.parse(ev.data) as {
            ts_ms: number
            symbols: Record<string, { mid: number; mid_units: number; price_scale: number }>
          }
          stopFallback()
          failureCount.current = 0
          retryMs = 250
          const out: Record<string, SymbolTick> = {}
          const dirs: Record<string, Direction> = {}
          for (const [sym, v] of Object.entries(body.symbols)) {
            out[sym] = {
              symbol: sym,
              mid: v.mid,
              midUnits: v.mid_units,
              priceScale: v.price_scale,
            }
            const prev = prevMids.current[sym]
            if (prev === undefined) dirs[sym] = 'flat'
            else if (v.mid > prev) dirs[sym] = 'up'
            else if (v.mid < prev) dirs[sym] = 'down'
            else dirs[sym] = 'flat'
            prevMids.current[sym] = v.mid
          }
          setTicks(out)
          setDirections(dirs)
        } catch {
          /* malformed frame — wait for the next one */
        }
      })
      es.onerror = () => {
        es?.close()
        es = null
        failureCount.current += 1
        if (DEMO && failureCount.current >= 2) startFallback()
        if (cancelled) return
        retryTimer = setTimeout(open, retryMs)
        retryMs = Math.min(retryMs * 2, 5_000)
      }
    }

    open()
    return () => {
      cancelled = true
      es?.close()
      if (retryTimer) clearTimeout(retryTimer)
      stopFallback()
    }
  }, [])

  const rows = useMemo(() => {
    return Object.values(ticks).sort((a, b) => a.symbol.localeCompare(b.symbol))
  }, [ticks])

  if (rows.length === 0) return null

  return (
    <div className="flex flex-wrap gap-x-6 gap-y-1 font-mono text-2xs uppercase tracking-wider">
      {rows.map((row) => {
        const dir = directions[row.symbol] ?? 'flat'
        const tone =
          dir === 'up' ? 'text-signal-live' : dir === 'down' ? 'text-signal-warn' : 'text-foreground'
        const arrow = dir === 'up' ? '▲' : dir === 'down' ? '▼' : '◆'
        return (
          <div key={row.symbol} className="flex items-baseline gap-2">
            <span className="text-muted-foreground">{row.symbol}</span>
            <span className={cn('font-semibold tabular-nums', tone)}>
              {formatPrice(row.mid)}
            </span>
            <span aria-hidden className={cn('text-[10px]', tone)}>
              {arrow}
            </span>
          </div>
        )
      })}
    </div>
  )
}

function formatPrice(p: number): string {
  if (p >= 10_000) return p.toFixed(0)
  if (p >= 100) return p.toFixed(2)
  if (p >= 1) return p.toFixed(4)
  return p.toFixed(6)
}
