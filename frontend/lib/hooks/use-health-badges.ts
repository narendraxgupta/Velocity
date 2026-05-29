/**
 * useHealthBadges — polls /v1/leaderboard/health for IsolationForest
 * verdicts and surfaces them as a Map<submission_id, HealthBadge>.
 *
 * The badge endpoint returns a stable point-in-time view of every
 * cached verdict. We poll on a 5-second cadence — anomaly verdicts
 * don't flip every frame and the leaderboard is already busy enough
 * with the per-submission row updates.
 *
 * The hook degrades silently: missing data, network errors, and stale
 * verdicts all surface as `health = 'ok'` (the visual ground state).
 * That's the right call for a *signal* that lives next to the primary
 * leaderboard rank — false positives are more user-hostile than
 * missed flags.
 */

'use client'

import { useEffect, useState } from 'react'

import { apiFetch } from '@/lib/api/client'

export type HealthBadge = {
  submission_id: string
  health: 'ok' | 'watch' | 'anomaly'
  rank_pct: number
  reason: string
  score?: number
}

export function useHealthBadges(intervalMs = 5_000) {
  const [badges, setBadges] = useState<Map<string, HealthBadge>>(new Map())

  useEffect(() => {
    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | null = null

    const poll = async () => {
      try {
        const r = await apiFetch(`/v1/leaderboard/health`, {
          cache: 'no-store',
        })
        if (!r.ok) {
          schedule()
          return
        }
        const body = (await r.json()) as { submissions: HealthBadge[] }
        if (cancelled) return
        const map = new Map<string, HealthBadge>()
        for (const b of body.submissions ?? []) {
          map.set(b.submission_id, b)
        }
        setBadges(map)
      } catch {
        // Network errors are not fatal — re-schedule and try again.
      }
      schedule()
    }
    const schedule = () => {
      if (cancelled) return
      timer = setTimeout(poll, intervalMs)
    }

    poll()
    return () => {
      cancelled = true
      if (timer) clearTimeout(timer)
    }
  }, [intervalMs])

  return badges
}
