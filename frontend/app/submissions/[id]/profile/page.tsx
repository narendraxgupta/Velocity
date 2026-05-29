/**
 * Submission profile page — interactive CPU flamegraph.
 *
 * Pulls `/v1/submissions/:id/flamegraph` and renders it via our native
 * SVG-based Flamegraph component. The endpoint returns the folded-stack
 * payload produced by the perf-profiler sidecar attached to the sandbox pod;
 * if the sidecar hasn't recorded anything yet (e.g. the submission was
 * deployed without `EnableProfiler`) we render a friendly empty state.
 */

'use client'

import { useParams } from 'next/navigation'
import { useEffect, useState } from 'react'

import { Flamegraph } from '@/components/profile/flamegraph'
import {
  Panel,
  PanelBody,
  PanelDescription,
  PanelHeader,
  PanelTitle,
} from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { isDemoMode } from '@/lib/demo-data'
import { middleTruncate } from '@/lib/utils'

type FlamegraphResponse = {
  submission_id:    string
  folded:           string
  recorded_at_ns:   number
  sample_freq_hz:   number
  duration_seconds: number
}

export default function ProfilePage() {
  const params = useParams<{ id: string }>()
  const id = params?.id ?? ''

  const [resp,  setResp]  = useState<FlamegraphResponse | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    if (!id) return
    let cancelled = false

    if (isDemoMode()) {
      setResp({
        submission_id:    id,
        folded:           DEMO_FOLDED,
        recorded_at_ns:   Date.now() * 1_000_000,
        sample_freq_hz:   99,
        duration_seconds: 35,
      })
      setLoading(false)
      return
    }

    setLoading(true)
    apiFetch(`/v1/submissions/${encodeURIComponent(id)}/flamegraph`, {
      cache: 'no-store',
    })
      .then(async (r) => {
        if (cancelled) return
        if (r.status === 404) {
          setError('No flamegraph recorded for this submission yet. Re-deploy with profiling enabled.')
          return
        }
        if (!r.ok) {
          setError(`HTTP ${r.status}`)
          return
        }
        const json = (await r.json()) as FlamegraphResponse
        setResp(json)
        setError(null)
      })
      .catch((e) => {
        if (cancelled) return
        setError((e as Error).message)
      })
      .finally(() => {
        if (!cancelled) setLoading(false)
      })

    return () => {
      cancelled = true
    }
  }, [id])

  return (
    <div className="container space-y-6 py-8">
      <header className="space-y-1">
        <span className="label-eyebrow">submission / profile</span>
        <h1 className="font-display text-2xl font-semibold tracking-tight">
          CPU flamegraph · {middleTruncate(id, 28)}
        </h1>
        <p className="text-sm text-muted-foreground">
          Recorded by the perf-profiler sidecar at{' '}
          {resp?.sample_freq_hz ?? 99} Hz across the entire benchmark window.
          Wider frames = more CPU time. Click any frame to zoom in; click again
          to reset.
        </p>
      </header>

      <Panel>
        <PanelHeader>
          <PanelTitle>Interactive flamegraph</PanelTitle>
          <PanelDescription>
            Brendan-Gregg-format folded stacks rendered as an SVG icicle.
            Hover for sample count + percentage. The leaves (top) are the
            functions actually on-CPU at the moment of sampling — those are
            the candidates for optimisation.
          </PanelDescription>
        </PanelHeader>
        <PanelBody>
          {loading && (
            <p className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
              Loading flamegraph…
            </p>
          )}
          {!loading && error && (
            <p className="rounded border border-signal-warn/40 bg-signal-warn/5 px-3 py-2 text-sm text-signal-warn">
              {error}
            </p>
          )}
          {!loading && !error && resp && (
            <Flamegraph folded={resp.folded} width={1280} />
          )}
        </PanelBody>
      </Panel>
    </div>
  )
}

/* -------------------------------------------------------------------------- */

// A synthetic folded payload used in demo mode. Roughly representative of a
// busy matching engine: ~70% in the event loop, with a long tail of
// allocator, networking, and validator frames.
const DEMO_FOLDED = `
engine_main;reactor_loop;handle_order;match;orderbook::insert 4200
engine_main;reactor_loop;handle_order;match;orderbook::erase 2100
engine_main;reactor_loop;handle_order;match;orderbook::price_level::push 1800
engine_main;reactor_loop;handle_order;match;orderbook::price_level::pop 1700
engine_main;reactor_loop;handle_order;parse_payload 1500
engine_main;reactor_loop;handle_order;parse_payload;simdjson::parse 900
engine_main;reactor_loop;handle_order;serialize_ack 700
engine_main;reactor_loop;handle_order;serialize_ack;flatbuffers::build 420
engine_main;reactor_loop;handle_order;validate;fifo_check 380
engine_main;reactor_loop;handle_order;validate;stp_check 220
engine_main;reactor_loop;handle_order;validate;tif_check 140
engine_main;reactor_loop;net::epoll_wait 1200
engine_main;reactor_loop;net::send 950
engine_main;reactor_loop;net::recv 880
engine_main;reactor_loop;telemetry::publish 600
engine_main;reactor_loop;telemetry::publish;kafka::produce 460
engine_main;reactor_loop;telemetry::publish;kafka::produce;snappy::compress 200
engine_main;mem::malloc 410
engine_main;mem::free 380
engine_main;mem::malloc;jemalloc::arena_extent_alloc 220
engine_main;gc;tlab_refill 95
engine_main;gc;safepoint 30
runtime;syscall;futex 240
runtime;syscall;mmap 60
runtime;syscall;munmap 45
`.trim()
