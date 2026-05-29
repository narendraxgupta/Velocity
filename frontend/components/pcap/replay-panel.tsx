/**
 * PcapReplayPanel — admin-pane control for firing a recorded .pcap at a
 * candidate submission engine.
 *
 * Hits the gateway routes:
 *   - GET  /v1/pcaps                            → list available captures
 *   - POST /v1/pcaps/replay                     → start replay
 *   - GET  /v1/pcaps/{benchmark_id}/state       → polled progress (every 1s)
 *
 * The right column shows whichever replay was kicked off most recently
 * in this session. We deliberately don't persist that across reloads —
 * if the operator wants long-running visibility, the benchmark id is
 * surfaced on success and they can navigate to the regular submission
 * detail page just like a fleet-driven run.
 */

'use client'

import { useCallback, useEffect, useState } from 'react'
import { toast } from 'sonner'

import {
  Panel,
  PanelBody,
  PanelDescription,
  PanelHeader,
  PanelTitle,
} from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'

interface PcapItem {
  benchmark_id: string
  object_key: string
  size_bytes: number
}

interface ReplayState {
  benchmark_id: string
  state: string
  detail?: string
  expected_packets: number
  sent_total: number
  acked_total: number
  errored_total: number
  current_rps: number
  p50_latency_ns: number
  p99_latency_ns: number
}

export function PcapReplayPanel() {
  const [pcaps, setPcaps] = useState<PcapItem[]>([])
  const [selected, setSelected] = useState<string>('')
  const [targetHost, setTargetHost] = useState('')
  const [targetPort, setTargetPort] = useState(8080)
  const [clockMode, setClockMode] = useState<'preserve' | 'fixed_rps'>('preserve')
  const [fixedRps, setFixedRps] = useState(50000)
  const [speed, setSpeed] = useState(1)
  const [busy, setBusy] = useState(false)
  const [replay, setReplay] = useState<ReplayState | null>(null)

  const refreshPcaps = useCallback(async () => {
    try {
      const resp = await apiFetch(`/v1/pcaps?limit=50`)
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`)
      const body = (await resp.json()) as { items?: PcapItem[] }
      const items = body.items ?? []
      setPcaps(items)
      const first = items[0]
      if (!selected && first) setSelected(first.object_key)
    } catch (err) {
      toast.error('Failed to list pcaps', { description: (err as Error).message })
    }
  }, [selected])

  useEffect(() => {
    refreshPcaps()
  }, [refreshPcaps])

  // Poll the current replay's state every second while it's running.
  useEffect(() => {
    if (!replay || replay.state !== 'running') return
    const id = setInterval(async () => {
      try {
        const resp = await apiFetch(
          `/v1/pcaps/${encodeURIComponent(replay.benchmark_id)}/state`,
        )
        if (!resp.ok) return
        const body = (await resp.json()) as ReplayState
        setReplay(body)
      } catch {
        // transient; keep polling
      }
    }, 1000)
    return () => clearInterval(id)
  }, [replay])

  const startReplay = useCallback(async () => {
    if (!selected || !targetHost || !targetPort) {
      toast.error('Missing required fields')
      return
    }
    setBusy(true)
    try {
      const resp = await apiFetch(`/v1/pcaps/replay`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          target_host: targetHost,
          target_port: targetPort,
          source_object_key: selected,
          clock_mode: clockMode,
          fixed_rps: clockMode === 'fixed_rps' ? fixedRps : 0,
          speed_multiplier: clockMode === 'preserve' ? speed : 1,
        }),
      })
      if (!resp.ok) {
        const detail = await resp.text()
        throw new Error(detail || `HTTP ${resp.status}`)
      }
      const body = (await resp.json()) as {
        benchmark_id: string
        expected_packets: number
      }
      setReplay({
        benchmark_id: body.benchmark_id,
        state: 'running',
        expected_packets: body.expected_packets,
        sent_total: 0,
        acked_total: 0,
        errored_total: 0,
        current_rps: 0,
        p50_latency_ns: 0,
        p99_latency_ns: 0,
      })
      toast.success('Replay started', {
        description: `${body.benchmark_id} — ${body.expected_packets.toLocaleString()} packets queued`,
      })
    } catch (err) {
      toast.error('Replay failed to start', { description: (err as Error).message })
    } finally {
      setBusy(false)
    }
  }, [selected, targetHost, targetPort, clockMode, fixedRps, speed])

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Pcap replay</PanelTitle>
        <PanelDescription>
          Re-fire a recorded TCP byte stream against a live submission. Use{' '}
          <code className="font-mono">preserve</code> to honour the original
          inter-packet gaps (best for bug bisect), or{' '}
          <code className="font-mono">fixed_rps</code> to load-test at a synthetic rate.
        </PanelDescription>
      </PanelHeader>

      <PanelBody>
        <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
          <Field label="Source pcap">
            <select
              className="form-input"
              value={selected}
              onChange={(e) => setSelected(e.target.value)}
            >
              {pcaps.length === 0 && <option value="">no pcaps available</option>}
              {pcaps.map((p) => (
                <option key={p.object_key} value={p.object_key}>
                  {p.benchmark_id} · {formatBytes(p.size_bytes)}
                </option>
              ))}
            </select>
          </Field>
          <Field label="Target host">
            <input
              required
              className="form-input"
              value={targetHost}
              onChange={(e) => setTargetHost(e.target.value)}
              placeholder="submission-engine.velocity-sandbox.svc.cluster.local"
            />
          </Field>
          <Field label="Target port">
            <input
              type="number"
              className="form-input"
              value={targetPort}
              onChange={(e) => setTargetPort(parseInt(e.target.value, 10) || 8080)}
            />
          </Field>
          <Field label="Clock mode">
            <select
              className="form-input"
              value={clockMode}
              onChange={(e) => setClockMode(e.target.value as 'preserve' | 'fixed_rps')}
            >
              <option value="preserve">preserve (real-time)</option>
              <option value="fixed_rps">fixed rps</option>
            </select>
          </Field>
          {clockMode === 'preserve' ? (
            <Field label="Speed multiplier">
              <input
                type="number"
                step="0.1"
                className="form-input"
                value={speed}
                onChange={(e) => setSpeed(parseFloat(e.target.value) || 1)}
              />
            </Field>
          ) : (
            <Field label="Fixed RPS">
              <input
                type="number"
                className="form-input"
                value={fixedRps}
                onChange={(e) => setFixedRps(parseInt(e.target.value, 10) || 1)}
              />
            </Field>
          )}
          <div className="flex items-end">
            <button
              type="button"
              onClick={startReplay}
              disabled={busy || !selected}
              className="btn-primary disabled:cursor-not-allowed disabled:opacity-50"
            >
              {busy ? 'Starting…' : 'Start replay'}
            </button>
          </div>
        </div>

        {replay && (
          <div className="mt-4 rounded-md border border-border bg-card/50 p-3 font-mono text-2xs">
            <div className="flex items-center justify-between">
              <span className="text-muted-foreground">
                {replay.benchmark_id}
              </span>
              <span
                className={
                  replay.state === 'running'
                    ? 'text-signal-live'
                    : replay.state === 'complete'
                    ? 'text-signal-info'
                    : 'text-signal-ask'
                }
              >
                {replay.state}
              </span>
            </div>
            <div className="mt-2 grid grid-cols-3 gap-2">
              <Stat label="sent">{replay.sent_total.toLocaleString()}</Stat>
              <Stat label="acked">{replay.acked_total.toLocaleString()}</Stat>
              <Stat label="errored">{replay.errored_total.toLocaleString()}</Stat>
              <Stat label="rps">{replay.current_rps.toLocaleString()}</Stat>
              <Stat label="p50">{formatLatencyNs(replay.p50_latency_ns)}</Stat>
              <Stat label="p99">{formatLatencyNs(replay.p99_latency_ns)}</Stat>
            </div>
            {replay.detail && (
              <div className="mt-2 text-signal-ask">{replay.detail}</div>
            )}
          </div>
        )}
      </PanelBody>
    </Panel>
  )
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label className="flex flex-col gap-1.5">
      <span className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
        {label}
      </span>
      {children}
    </label>
  )
}

function Stat({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div>
      <div className="text-muted-foreground">{label}</div>
      <div className="text-foreground">{children}</div>
    </div>
  )
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MiB`
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GiB`
}

function formatLatencyNs(ns: number): string {
  if (ns === 0) return '—'
  if (ns < 1_000) return `${ns}ns`
  if (ns < 1_000_000) return `${(ns / 1000).toFixed(1)}µs`
  if (ns < 1_000_000_000) return `${(ns / 1_000_000).toFixed(1)}ms`
  return `${(ns / 1_000_000_000).toFixed(2)}s`
}
