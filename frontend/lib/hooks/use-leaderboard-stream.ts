/**
 * useLeaderboardStream — a small React hook around the leaderboard-ws
 * service. Handles:
 *
 *   - REST hydration on mount (so the board is populated immediately on
 *     load, not just when the next score change happens)
 *   - automatic reconnect with capped exponential backoff
 *   - subscribing to a division (or "global")
 *   - JSON parsing into typed Row records
 *   - merging incremental updates into a stable, sorted list
 *
 * Server contract (authoritative — see scoring-service
 * leaderboard_publisher.cpp and the api-gateway leaderboard.cpp `top` route)
 * ----------------------------------------------------------------------------
 * The WebSocket (leaderboard-ws) forwards the scoring service's Redis pub/sub
 * payload verbatim. The scoring service publishes ONLY incremental deltas:
 *
 *   { "type": "hello",  "stream": "global" }            // on connect
 *   { "type": "leaderboard.delta", "ts_ns": …, "upserts": [ …row… ] }
 *
 * The HTTP read path (`GET /v1/leaderboard?n=`) returns the full current
 * ranking as `{ "entries": [ …row… ] }` and is what we hydrate from on mount.
 *
 * Both the delta upserts and the REST entries use the SAME field names:
 *
 *   {
 *     "rank"?:             number,   // present on REST, absent on deltas
 *     "submission_id":     string,
 *     "display_name":      string,
 *     "team_name":         string,
 *     "composite_score":   number,
 *     "throughput_score":  number,
 *     "latency_score":     number,
 *     "correctness_score": number,
 *     "sustained_rps":     number,
 *     "p50_ns":            number,
 *     "p99_ns":            number,
 *     "updated_at_ns":     number,
 *   }
 *
 * Deltas don't carry rank / status / delta, so we derive rank client-side by
 * sorting on composite (descending) and compute the Δ from the previous rank.
 */

'use client'

import { useEffect, useMemo, useRef, useState } from 'react'

import { apiFetch } from '@/lib/api/client'
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

/**
 * The wire shape shared by the REST `entries` payload and the WS `upserts`
 * payload. Numbers may arrive as strings from the Redis-hash read path, so we
 * coerce defensively. Legacy field aliases (`team`, `throughput_rps`,
 * `correctness`) are accepted too in case an older publisher is in front of us.
 */
type WireEntry = {
  rank?: number | string
  submission_id: string
  display_name?: string
  team_name?: string
  team?: string
  composite_score?: number | string
  throughput_score?: number | string
  latency_score?: number | string
  correctness_score?: number | string
  correctness?: number | string
  sustained_rps?: number | string
  throughput_rps?: number | string
  p50_ns?: number | string
  p99_ns?: number | string
  updated_at_ns?: number | string
  status?: LeaderRow['status']
  delta?: number | string
}

type WireMessage =
  | { type: 'hello'; stream: string }
  | { type: 'subscribed'; stream: string }
  | { type: 'leaderboard.delta'; ts_ns?: number; upserts?: WireEntry[] }
  // Back-compat with the originally-designed (never-shipped) envelope.
  | { type: 'snapshot'; rows?: WireEntry[] }
  | { type: 'tick'; rows?: WireEntry[] }

export type ConnectionState = 'connecting' | 'open' | 'closed' | 'error'

// Resolve the leaderboard WebSocket URL at RUNTIME (in the browser) rather
// than baking a host into the bundle at build time.
//
//  1. An explicit, non-empty NEXT_PUBLIC_LEADERBOARD_WS always wins.
//  2. Otherwise derive from the page's own location so the same image works
//     locally AND behind the GitHub Codespaces tunnel, where each forwarded
//     container port lives on its own subdomain "<name>-<port>.app.github.dev".
//     We swap the frontend's port segment (3000) for the leaderboard-ws port.
//  3. SSR fallback (no window) — only used if a client never hydrates.
const LEADERBOARD_WS_PORT = '8090'
const LEADERBOARD_WS_PATH = '/v1/leaderboard'

function resolveLeaderboardWsUrl(): string {
  const explicit = process.env.NEXT_PUBLIC_LEADERBOARD_WS
  if (explicit && explicit.length > 0) return explicit

  if (typeof window === 'undefined') {
    return `ws://localhost:${LEADERBOARD_WS_PORT}${LEADERBOARD_WS_PATH}`
  }

  const { protocol, hostname } = window.location
  const wsProto = protocol === 'https:' ? 'wss:' : 'ws:'

  // GitHub Codespaces / preview domains: "<name>-<port>.<domain>".
  const codespace = hostname.match(
    /^(.*)-(\d+)\.(app\.github\.dev|githubpreview\.dev)$/,
  )
  if (codespace) {
    const [, prefix, , domain] = codespace
    return `${wsProto}//${prefix}-${LEADERBOARD_WS_PORT}.${domain}${LEADERBOARD_WS_PATH}`
  }

  // Local dev / direct host: same hostname, leaderboard-ws port.
  return `${wsProto}//${hostname}:${LEADERBOARD_WS_PORT}${LEADERBOARD_WS_PATH}`
}

export function useLeaderboardStream(stream: string = 'global') {
  const [rows, setRows] = useState<LeaderRow[]>([])
  const [state, setState] = useState<ConnectionState>('connecting')
  const [updatedAt, setUpdatedAt] = useState<number | null>(null)

  const wsRef = useRef<WebSocket | null>(null)
  const reconnectAt = useRef<number>(500)
  // Last-known rank per submission, so we can compute the Δ column across
  // re-rankings even though the wire never sends it.
  const prevRanks = useRef<Map<string, number>>(new Map())

  useEffect(() => {
    setRows([])
    setState('connecting')
    setUpdatedAt(null)
    prevRanks.current = new Map()

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

    // Merge `incoming` rows into the current list (by submission_id), re-rank
    // by composite descending, and stamp the Δ from the prior ranking.
    const applyUpserts = (incoming: LeaderRow[], replace = false) => {
      if (cancelled) return
      setRows((current) => {
        const byId = new Map(replace ? [] : current.map((r) => [r.submissionId, r]))
        for (const r of incoming) byId.set(r.submissionId, r)
        const ranked = Array.from(byId.values())
          .sort((a, b) => b.composite - a.composite)
          .map((r, i) => {
            const newRank = i + 1
            const prior = prevRanks.current.get(r.submissionId)
            return {
              ...r,
              rank: newRank,
              delta: prior != null ? prior - newRank : 0,
            }
          })
        prevRanks.current = new Map(ranked.map((r) => [r.submissionId, r.rank]))
        return ranked
      })
      setUpdatedAt(Date.now())
    }

    // Cold-start hydration: pull the full ranking over HTTP so the table has
    // content immediately (the WS only pushes future deltas). Also re-run on
    // reconnect to recover any deltas missed while disconnected.
    const hydrate = async () => {
      try {
        const res = await apiFetch(`/v1/leaderboard?n=100`, { cache: 'no-store' })
        if (!res.ok) return
        const data = (await res.json()) as { entries?: WireEntry[] }
        if (cancelled) return
        applyUpserts((data.entries ?? []).map(toRow), /*replace=*/true)
      } catch {
        // Gateway unreachable — the WS path may still deliver deltas.
      }
    }

    const connect = () => {
      if (cancelled) return
      setState('connecting')
      let ws: WebSocket
      try {
        ws = new WebSocket(resolveLeaderboardWsUrl())
      } catch {
        // Malformed URL or blocked — fall back to REST polling cadence.
        setState('error')
        timer = setTimeout(connect, Math.min(reconnectAt.current, 5_000))
        reconnectAt.current = Math.min(reconnectAt.current * 2, 5_000)
        return
      }
      wsRef.current = ws

      ws.onopen = () => {
        if (cancelled) {
          ws.close()
          return
        }
        setState('open')
        reconnectAt.current = 500
        ws.send(JSON.stringify({ subscribe: stream }))
        void hydrate()
      }

      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data as string) as WireMessage
          if (msg.type === 'leaderboard.delta') {
            applyUpserts((msg.upserts ?? []).map(toRow))
          } else if (msg.type === 'snapshot') {
            applyUpserts((msg.rows ?? []).map(toRow), /*replace=*/true)
          } else if (msg.type === 'tick') {
            applyUpserts((msg.rows ?? []).map(toRow))
          }
          // hello / subscribed carry no rows — ignored.
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

    // Hydrate first so the board is never blank while the socket handshakes.
    void hydrate()
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

function toRow(w: WireEntry): LeaderRow {
  return {
    rank: num(w.rank),
    team: w.team_name || w.display_name || w.team || w.submission_id,
    submissionId: w.submission_id,
    composite: num(w.composite_score),
    throughputRps: num(w.sustained_rps ?? w.throughput_rps),
    p99Ns: num(w.p99_ns),
    correctness: num(w.correctness_score ?? w.correctness),
    status: w.status ?? 'scored',
    delta: num(w.delta),
  }
}

function num(v: number | string | undefined | null): number {
  if (typeof v === 'number') return Number.isFinite(v) ? v : 0
  if (typeof v === 'string') {
    const n = Number(v)
    return Number.isFinite(n) ? n : 0
  }
  return 0
}

function clamp(lo: number, hi: number, x: number): number {
  return Math.max(lo, Math.min(hi, x))
}
