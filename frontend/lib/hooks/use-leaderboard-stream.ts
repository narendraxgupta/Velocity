/**
 * useLeaderboardStream — a small React hook around the leaderboard-ws
 * service. Handles:
 *
 *   - automatic reconnect with capped exponential backoff
 *   - subscribing to a division (or "global")
 *   - JSON parsing into typed Row records
 *   - merging incremental updates into a stable, sorted list
 *
 * Server contract
 * ---------------
 * The server pushes one of two message kinds:
 *
 *   { "type": "hello",  "stream": "global" }
 *   { "type": "snapshot", "rows": [...] }   <-- full state
 *   { "type": "tick",     "rows": [...] }   <-- delta updates by submission_id
 *
 * Each row is:
 *
 *   {
 *     "rank":               number,
 *     "team":               string,
 *     "submission_id":      string,
 *     "composite_score":    number,
 *     "throughput_rps":     number,
 *     "p99_ns":             number,
 *     "correctness":        number,
 *     "status":             "running" | "scored" | "queued" | "dq",
 *     "delta":              number,
 *   }
 */

'use client'

import { useEffect, useMemo, useRef, useState } from 'react'

import { isDemoMode, mockLeaderboard } from '@/lib/demo-data'

export type LeaderRow = {
  rank: number
  team: string
  submissionId: string
  composite: number
  throughputRps: number
  p99Ns: number
  correctness: number
  status: 'running' | 'scored' | 'queued' | 'dq'
  delta: number
}

type WireRow = {
  rank: number
  team: string
  submission_id: string
  composite_score: number
  throughput_rps: number
  p99_ns: number
  correctness: number
  status: LeaderRow['status']
  delta: number
}

type WireMessage =
  | { type: 'hello'; stream: string }
  | { type: 'snapshot'; rows: WireRow[] }
  | { type: 'tick'; rows: WireRow[] }
  | { type: 'subscribed'; stream: string }

export type ConnectionState = 'connecting' | 'open' | 'closed' | 'error'

const DEFAULT_URL = process.env.NEXT_PUBLIC_LEADERBOARD_WS ?? 'ws://localhost:8090/v1/leaderboard'

export function useLeaderboardStream(stream: string = 'global') {
  const [rows, setRows] = useState<LeaderRow[]>([])
  const [state, setState] = useState<ConnectionState>('connecting')
  const [updatedAt, setUpdatedAt] = useState<number | null>(null)

  const wsRef = useRef<WebSocket | null>(null)
  const reconnectAt = useRef<number>(500)

  useEffect(() => {
    setRows([])
    setState('connecting')
    setUpdatedAt(null)

    // Demo / canned-data mode. Emit a snapshot immediately, then wiggle the
    // top of the board every second so the UI feels alive without a backend.
    if (isDemoMode()) {
      let base = mockLeaderboard()
      setRows(base)
      setState('open')
      setUpdatedAt(Date.now())
      const ticker = setInterval(() => {
        base = base.map((r, i) => ({
          ...r,
          composite: i < 2
            ? clamp(0, 100, r.composite + (Math.random() - 0.5) * 0.4)
            : r.composite,
          p99Ns: i < 3
            ? Math.max(15_000, r.p99Ns + Math.floor((Math.random() - 0.5) * 1_400))
            : r.p99Ns,
          delta: i === 0 ? 0 : Math.floor((Math.random() - 0.5) * 2),
        }))
        // Re-sort and re-rank.
        base = base
          .sort((a, b) => b.composite - a.composite)
          .map((r, i) => ({ ...r, rank: i + 1 }))
        setRows(base)
        setUpdatedAt(Date.now())
      }, 1_000)
      return () => clearInterval(ticker)
    }

    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | null = null

    const connect = () => {
      if (cancelled) return
      setState('connecting')
      const ws = new WebSocket(DEFAULT_URL)
      wsRef.current = ws

      ws.onopen = () => {
        if (cancelled) {
          ws.close()
          return
        }
        setState('open')
        reconnectAt.current = 500
        ws.send(JSON.stringify({ subscribe: stream }))
      }

      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data as string) as WireMessage
          if (msg.type === 'snapshot') {
            setRows(msg.rows.map(toRow).sort((a, b) => a.rank - b.rank))
            setUpdatedAt(Date.now())
          } else if (msg.type === 'tick') {
            setRows((current) => mergeTick(current, msg.rows.map(toRow)))
            setUpdatedAt(Date.now())
          }
        } catch {
          // malformed payload — ignore
        }
      }

      ws.onerror = () => {
        setState('error')
      }

      ws.onclose = () => {
        if (cancelled) return
        setState('closed')
        const delay = Math.min(reconnectAt.current, 5_000)
        reconnectAt.current = Math.min(reconnectAt.current * 2, 5_000)
        timer = setTimeout(connect, delay)
      }
    }

    connect()
    return () => {
      cancelled = true
      if (timer) clearTimeout(timer)
      wsRef.current?.close()
    }
  }, [stream])

  return useMemo(() => ({ rows, state, updatedAt }), [rows, state, updatedAt])
}

/* -------------------------------------------------------------------------- */

function toRow(w: WireRow): LeaderRow {
  return {
    rank: w.rank,
    team: w.team,
    submissionId: w.submission_id,
    composite: w.composite_score,
    throughputRps: w.throughput_rps,
    p99Ns: w.p99_ns,
    correctness: w.correctness,
    status: w.status,
    delta: w.delta,
  }
}

function clamp(lo: number, hi: number, x: number): number {
  return Math.max(lo, Math.min(hi, x))
}

function mergeTick(current: LeaderRow[], deltas: LeaderRow[]): LeaderRow[] {
  const byId = new Map(current.map((r) => [r.submissionId, r]))
  for (const d of deltas) byId.set(d.submissionId, d)
  return Array.from(byId.values()).sort((a, b) => a.rank - b.rank)
}
