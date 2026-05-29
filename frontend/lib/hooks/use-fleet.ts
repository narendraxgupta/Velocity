/**
 * useFleet — polls /v1/fleet for the current registry of bot workers.
 *
 * The endpoint is cheap (single Redis GET) so we poll every 2s; if it
 * gets expensive in the future, swap to a Redis-backed pubsub channel
 * and stream it via SSE.
 */

'use client'

import { useEffect, useMemo, useState } from 'react'

import { apiFetch } from '@/lib/api/client'
import { isDemoMode } from '@/lib/demo-data'

export type Worker = {
  workerId: string
  hostname: string
  cpuCount: number
  sentTotal: number
  ackedTotal: number
  erroredTotal: number
  currentRps: number
  activeBots: number
  cpuPercent: number
  lastHeartbeatNs: number
  healthy: boolean
  errorRate: number
}

type WireWorker = {
  worker_id: string
  hostname: string
  cpu_count: number
  sent_total: number
  acked_total: number
  errored_total: number
  current_rps: number
  active_bots: number
  cpu_percent: number
  last_heartbeat_ns: number
}

export function useFleet() {
  const [workers, setWorkers] = useState<Worker[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [updatedAt, setUpdatedAt] = useState<number | null>(null)

  useEffect(() => {
    let cancelled = false

    if (isDemoMode()) {
      const tick = () => {
        if (cancelled) return
        setWorkers(mockFleet())
        setUpdatedAt(Date.now())
      }
      tick()
      const id = setInterval(tick, 2000)
      return () => {
        cancelled = true
        clearInterval(id)
      }
    }

    const fetchOnce = async () => {
      try {
        const res = await apiFetch(`/v1/fleet`, { cache: 'no-store' })
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        const data = (await res.json()) as { workers: WireWorker[] }
        if (cancelled) return
        const now = Date.now() * 1_000_000
        const mapped = (data.workers ?? []).map((w): Worker => {
          const sent = w.sent_total ?? 0
          const errored = w.errored_total ?? 0
          const lastHeartbeatNs = w.last_heartbeat_ns ?? 0
          const ageNs = now - lastHeartbeatNs
          const healthy = lastHeartbeatNs > 0 && ageNs < 5_000_000_000
          const denom = Math.max(1, sent)
          return {
            workerId: w.worker_id,
            hostname: w.hostname,
            cpuCount: w.cpu_count,
            sentTotal: sent,
            ackedTotal: w.acked_total ?? 0,
            erroredTotal: errored,
            currentRps: w.current_rps ?? 0,
            activeBots: w.active_bots ?? 0,
            cpuPercent: w.cpu_percent ?? 0,
            lastHeartbeatNs,
            healthy,
            errorRate: errored / denom,
          }
        })
        setWorkers(mapped)
        setUpdatedAt(Date.now())
        setError(null)
      } catch (e) {
        if (!cancelled) setError(String(e))
      }
    }

    fetchOnce()
    const id = setInterval(fetchOnce, 2000)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  return useMemo(
    () => ({ workers, error, updatedAt }),
    [workers, error, updatedAt],
  )
}

/* -------------------------------------------------------------------------- */

function mockFleet(): Worker[] {
  const now = Date.now() * 1_000_000
  return Array.from({ length: 24 }, (_, i) => {
    const sent = 1_000_000 + Math.floor(Math.random() * 4_000_000)
    const err = Math.floor(sent * (i === 7 ? 0.05 : Math.random() * 0.01))
    return {
      workerId: `bw-${i.toString().padStart(2, '0')}`,
      hostname: `node-${(i % 6) + 1}`,
      cpuCount: 4,
      sentTotal: sent,
      ackedTotal: sent - err,
      erroredTotal: err,
      currentRps: 12_000 + Math.floor(Math.random() * 8_000),
      activeBots: 256,
      cpuPercent: 40 + Math.floor(Math.random() * 50),
      lastHeartbeatNs: now - (i === 13 ? 8_000_000_000 : Math.floor(Math.random() * 500_000_000)),
      healthy: i !== 13,
      errorRate: err / Math.max(1, sent),
    }
  })
}
