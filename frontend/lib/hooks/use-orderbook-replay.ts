/**
 * useOrderbookReplay — fetches the timeline of orderbook snapshots for a
 * submission and lazily resolves the snapshot closest to a user-supplied
 * elapsed_ms (driven by the timeline scrubber on the replay viewer).
 *
 * Caching: we keep an in-memory LRU of the last 32 snapshots so the
 * scrubber feels instant when the user drags back-and-forth across a
 * narrow window. Once the timeline is loaded we stop polling.
 */

'use client'

import { useCallback, useEffect, useMemo, useRef, useState } from 'react'

import { apiFetch } from '@/lib/api/client'

export type DepthRow = { price: number; qty: number }

export type OrderbookSnapshot = {
  elapsed_ms: number
  bids: DepthRow[]
  asks: DepthRow[]
}

interface State {
  timeline: number[]
  current: OrderbookSnapshot | null
  loading: boolean
  error: string | null
}

const LRU_CAP = 32

export function useOrderbookReplay(submissionId: string | undefined) {
  const [state, setState] = useState<State>({
    timeline: [],
    current: null,
    loading: false,
    error: null,
  })
  const cache = useRef<Map<number, OrderbookSnapshot>>(new Map())
  const inFlight = useRef<AbortController | null>(null)

  // Bootstrap the timeline once on mount.
  useEffect(() => {
    if (!submissionId) return undefined
    cache.current.clear()
    if (inFlight.current) {
      inFlight.current.abort()
      inFlight.current = null
    }
    setState({ timeline: [], current: null, loading: false, error: null })
    let cancelled = false
    const tick = async () => {
      try {
        const resp = await apiFetch(
          `/v1/submissions/${encodeURIComponent(submissionId)}/orderbook/timeline`,
          { cache: 'no-store' },
        )
        if (!resp.ok) {
          if (!cancelled) setState((s) => ({ ...s, error: `HTTP ${resp.status}` }))
          return
        }
        const body = (await resp.json()) as { samples: number[] }
        if (cancelled) return
        setState((s) => ({ ...s, timeline: body.samples, error: null }))
      } catch (err) {
        if (!cancelled) setState((s) => ({ ...s, error: (err as Error).message }))
      }
    }
    tick()
    // Keep polling slowly while the run is active; stops being useful
    // once the submission finishes but the cost is negligible.
    const id = setInterval(tick, 5_000)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [submissionId])

  const seek = useCallback(
    async (elapsedMs: number) => {
      if (!submissionId) return
      // Snap to the closest known sample to avoid 404s near the edges.
      const tl = state.timeline
      let target = elapsedMs
      if (tl.length > 0) {
        let bestDelta = Number.POSITIVE_INFINITY
        for (const s of tl) {
          const d = Math.abs(s - elapsedMs)
          if (d < bestDelta) { bestDelta = d; target = s }
        }
      }

      if (cache.current.has(target)) {
        setState((s) => ({ ...s, current: cache.current.get(target)! }))
        return
      }

      // Cancel any earlier in-flight fetch — only the latest seek matters.
      if (inFlight.current) inFlight.current.abort()
      const ac = new AbortController()
      inFlight.current = ac
      setState((s) => ({ ...s, loading: true }))
      try {
        const resp = await apiFetch(
          `/v1/submissions/${encodeURIComponent(submissionId)}/orderbook?elapsed_ms=${target}`,
          { cache: 'no-store', signal: ac.signal },
        )
        if (!resp.ok) {
          setState((s) => ({ ...s, loading: false, error: `HTTP ${resp.status}` }))
          return
        }
        const snap = (await resp.json()) as OrderbookSnapshot
        cache.current.set(snap.elapsed_ms, snap)
        // LRU trim.
        if (cache.current.size > LRU_CAP) {
          const first = cache.current.keys().next().value
          if (first !== undefined) cache.current.delete(first)
        }
        setState((s) => ({ ...s, current: snap, loading: false, error: null }))
      } catch (err) {
        if ((err as Error).name === 'AbortError') return
        setState((s) => ({ ...s, loading: false, error: (err as Error).message }))
      }
    },
    [submissionId, state.timeline],
  )

  // Auto-seek to the latest snapshot whenever the timeline grows.
  useEffect(() => {
    if (state.current === null && state.timeline.length > 0) {
      seek(state.timeline[state.timeline.length - 1]!)
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.timeline.length])

  return useMemo(
    () => ({
      timeline: state.timeline,
      current: state.current,
      loading: state.loading,
      error: state.error,
      seek,
    }),
    [state, seek],
  )
}
