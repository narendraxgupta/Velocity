/**
 * ChaosPanel — operator-facing UI for the chaos-orchestrator.
 *
 * Sits in the admin page. Exposes the five primitives the orchestrator
 * supports and shows the current active-injection table. Every action is
 * time-bounded; the panel reads the orchestrator's `/v1/chaos/status`
 * every 5 seconds so the operator can see injections fade out as their
 * deadlines pass.
 */

'use client'

import { useCallback, useEffect, useState } from 'react'
import { Skull, Wifi, WifiOff, Cpu, Network, RefreshCw } from 'lucide-react'
import { toast } from 'sonner'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'
import { isDemoMode } from '@/lib/demo-data'
import { cn } from '@/lib/utils'

type ActiveInjection = {
  kind:        string
  pod:         string
  namespace:   string
  expires_at:  string
  hints?:      Record<string, string>
}

type StatusResp = {
  allowed_namespaces: string
  active:             Record<string, ActiveInjection>
  server_ts_ns:       number
}

type Action =
  | { kind: 'pod-kill' }
  | { kind: 'tc-latency'; delayMs: number; jitterMs: number }
  | { kind: 'tc-loss'; lossPct: number }
  | { kind: 'cpu-throttle'; cpuMaxMicros: number }
  | { kind: 'partition'; upstream: string }

const ACTIONS = [
  {
    id: 'pod-kill',
    label: 'Pod kill',
    icon: Skull,
    description: 'Hard-kill the pod (grace=0) to verify stateless failover.',
    defaults: {},
  },
  {
    id: 'tc-latency',
    label: 'Network latency',
    icon: Wifi,
    description: 'Inject `tc netem delay` for the duration. Tail-latency stress.',
    defaults: { delayMs: 100, jitterMs: 20 },
  },
  {
    id: 'tc-loss',
    label: 'Packet loss',
    icon: WifiOff,
    description: 'Inject `tc netem loss` — drop X % of egress packets.',
    defaults: { lossPct: 5 },
  },
  {
    id: 'cpu-throttle',
    label: 'CPU throttle',
    icon: Cpu,
    description: 'Tighten the pod cgroup cpu.max so the engine runs starved.',
    defaults: { cpuMaxMicros: 50_000 },
  },
  {
    id: 'partition',
    label: 'Egress partition',
    icon: Network,
    description: 'iptables DROP egress to a named upstream. Default: redpanda.',
    defaults: { upstream: 'redpanda.velocity-data.svc.cluster.local' },
  },
] as const

export function ChaosPanel() {
  const [ns,  setNs]  = useState('velocity-sandbox')
  const [pod, setPod] = useState('')
  const [actionId, setActionId] =
    useState<typeof ACTIONS[number]['id']>('tc-latency')
  const [delayMs,  setDelayMs]  = useState(100)
  const [jitterMs, setJitterMs] = useState(20)
  const [lossPct,  setLossPct]  = useState(5)
  const [cpuMax,   setCpuMax]   = useState(50_000)
  const [upstream, setUpstream] = useState('redpanda.velocity-data.svc.cluster.local')
  const [duration, setDuration] = useState(30)
  const [busy,     setBusy]     = useState(false)
  const [status,   setStatus]   = useState<StatusResp | null>(null)

  const refresh = useCallback(async () => {
    if (isDemoMode()) {
      setStatus({
        allowed_namespaces: 'velocity-sandbox,velocity-load',
        active: {},
        server_ts_ns: Date.now() * 1e6,
      })
      return
    }
    try {
      const r = await apiFetch(`/v1/chaos/status`, { cache: 'no-store' })
      if (!r.ok) return
      setStatus(await r.json())
    } catch {
      /* network blip — keep showing the last good snapshot */
    }
  }, [])

  useEffect(() => {
    refresh()
    const t = setInterval(refresh, 5_000)
    return () => clearInterval(t)
  }, [refresh])

  const inject = useCallback(async () => {
    if (!pod) {
      toast.error('Pod name required')
      return
    }
    setBusy(true)
    try {
      if (isDemoMode()) {
        toast(`Demo: would inject ${actionId} into ${ns}/${pod} for ${duration}s`)
        setBusy(false)
        return
      }
      let path = ''
      let body: Record<string, unknown> = {
        namespace: ns, pod, duration_s: duration,
      }
      switch (actionId) {
        case 'pod-kill':
          path = '/v1/chaos/pod-kill'
          break
        case 'tc-latency':
          path = '/v1/chaos/tc-latency'
          body = { ...body, delay_ms: delayMs, jitter_ms: jitterMs }
          break
        case 'tc-loss':
          path = '/v1/chaos/tc-loss'
          body = { ...body, loss_pct: lossPct }
          break
        case 'cpu-throttle':
          path = '/v1/chaos/cpu-throttle'
          body = { ...body, cpu_max_micros: cpuMax }
          break
        case 'partition':
          path = '/v1/chaos/partition'
          body = { ...body, upstream }
          break
      }
      const r = await apiFetch(path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      })
      const respBody = await r.json().catch(() => ({}))
      if (!r.ok) {
        throw new Error(respBody.error ?? `HTTP ${r.status}`)
      }
      toast.success(`${actionId} injected`, {
        description: `${ns}/${pod} for ${duration}s`,
      })
      refresh()
    } catch (e) {
      toast.error('Chaos injection failed', { description: String(e) })
    } finally {
      setBusy(false)
    }
  }, [actionId, ns, pod, duration, delayMs, jitterMs, lossPct, cpuMax, upstream, refresh])

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Chaos lab</PanelTitle>
        <PanelDescription>
          Inject a time-bounded fault into the running platform. Each action
          requires `pod` + `namespace`; the orchestrator's RBAC limits which
          namespaces are touchable —{' '}
          <span className="font-mono text-foreground">
            {status?.allowed_namespaces ?? '…'}
          </span>.
        </PanelDescription>
      </PanelHeader>
      <PanelBody className="space-y-4">
        <div className="grid grid-cols-2 gap-3">
          <Field label="Namespace">
            <input
              value={ns}
              onChange={(e) => setNs(e.target.value.trim())}
              className="w-full rounded border border-border bg-surface px-2.5 py-1.5 font-mono text-sm outline-none focus:border-accent"
            />
          </Field>
          <Field label="Pod">
            <input
              value={pod}
              onChange={(e) => setPod(e.target.value.trim())}
              placeholder="velocity-submission-01HQE…"
              className="w-full rounded border border-border bg-surface px-2.5 py-1.5 font-mono text-sm outline-none focus:border-accent"
            />
          </Field>
        </div>

        <div className="grid grid-cols-1 gap-2 sm:grid-cols-2 lg:grid-cols-5">
          {ACTIONS.map((a) => {
            const Icon = a.icon
            return (
              <button
                key={a.id}
                type="button"
                onClick={() => setActionId(a.id)}
                className={cn(
                  'flex flex-col items-start gap-1 rounded-md border px-3 py-2.5 text-left transition-colors',
                  actionId === a.id
                    ? 'border-accent bg-accent/10 text-foreground'
                    : 'border-border bg-surface text-muted-foreground hover:border-border-strong hover:text-foreground',
                )}
                title={a.description}
              >
                <Icon className="h-4 w-4" />
                <span className="font-mono text-2xs uppercase tracking-widest">
                  {a.label}
                </span>
              </button>
            )
          })}
        </div>

        {/* Action-specific knobs */}
        <div className="space-y-2 rounded-md border border-border bg-surface-subtle p-3">
          {actionId === 'tc-latency' && (
            <div className="grid grid-cols-2 gap-3">
              <Field label="Delay (ms)">
                <NumberInput value={delayMs} onChange={setDelayMs} min={1} />
              </Field>
              <Field label="Jitter (ms)">
                <NumberInput value={jitterMs} onChange={setJitterMs} min={0} />
              </Field>
            </div>
          )}
          {actionId === 'tc-loss' && (
            <Field label="Loss (%)">
              <NumberInput value={lossPct} onChange={setLossPct} min={1} max={100} />
            </Field>
          )}
          {actionId === 'cpu-throttle' && (
            <Field label="cpu.max numerator (µs of CPU per 100ms)">
              <NumberInput value={cpuMax} onChange={setCpuMax} min={1000} />
            </Field>
          )}
          {actionId === 'partition' && (
            <Field label="Upstream host or IP">
              <input
                value={upstream}
                onChange={(e) => setUpstream(e.target.value.trim())}
                className="w-full rounded border border-border bg-surface px-2.5 py-1.5 font-mono text-sm outline-none focus:border-accent"
              />
            </Field>
          )}
          {actionId === 'pod-kill' && (
            <p className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
              No tuning — sends DELETE pod with grace=0.
            </p>
          )}

          <Field label="Duration (seconds) — ignored for pod-kill">
            <NumberInput value={duration} onChange={setDuration} min={1} max={600} />
          </Field>
        </div>

        <div className="flex items-center gap-3">
          <button
            type="button"
            onClick={inject}
            disabled={busy || !pod}
            className="rounded-md bg-signal-warn px-4 py-2 text-sm font-semibold text-background transition-opacity disabled:opacity-40"
          >
            {busy ? 'Injecting…' : 'Inject chaos'}
          </button>
          <button
            type="button"
            onClick={refresh}
            className="inline-flex items-center gap-1.5 rounded border border-border bg-surface px-3 py-2 text-xs text-muted-foreground hover:text-foreground"
          >
            <RefreshCw className="h-3 w-3" />
            Refresh status
          </button>
        </div>

        {/* Active injections table */}
        <ActiveTable status={status} />
      </PanelBody>
    </Panel>
  )
}

/* -------------------------------------------------------------------------- */

function Field({
  label,
  children,
}: {
  label: string
  children: React.ReactNode
}) {
  return (
    <label className="block space-y-1">
      <span className="label-eyebrow">{label}</span>
      {children}
    </label>
  )
}

function NumberInput({
  value,
  onChange,
  min,
  max,
}: {
  value: number
  onChange: (v: number) => void
  min?: number
  max?: number
}) {
  return (
    <input
      type="number"
      value={value}
      onChange={(e) => {
        const v = Number(e.target.value)
        if (Number.isFinite(v)) onChange(v)
      }}
      min={min}
      max={max}
      className="w-full rounded border border-border bg-surface px-2.5 py-1.5 font-mono text-sm outline-none focus:border-accent"
    />
  )
}

function ActiveTable({ status }: { status: StatusResp | null }) {
  const entries = status ? Object.entries(status.active) : []
  if (entries.length === 0) {
    return (
      <p className="rounded border border-dashed border-border bg-surface px-3 py-2 font-mono text-2xs uppercase tracking-widest text-muted-foreground">
        No active injections
      </p>
    )
  }
  return (
    <table className="w-full overflow-hidden rounded border border-border bg-surface text-sm">
      <thead className="bg-surface-subtle">
        <tr className="text-left">
          <Th>Kind</Th>
          <Th>Target</Th>
          <Th>Expires</Th>
          <Th>Hints</Th>
        </tr>
      </thead>
      <tbody>
        {entries.map(([key, a]) => (
          <tr key={key} className="border-t border-border-subtle">
            <Td><span className="font-mono">{a.kind}</span></Td>
            <Td><span className="font-mono">{a.namespace}/{a.pod}</span></Td>
            <Td>
              <span className="font-mono text-2xs text-muted-foreground">
                {a.expires_at}
              </span>
            </Td>
            <Td>
              <span className="font-mono text-2xs text-muted-foreground">
                {a.hints ? Object.entries(a.hints).map(([k, v]) => `${k}=${v}`).join(', ') : '—'}
              </span>
            </Td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

function Th({ children }: { children: React.ReactNode }) {
  return (
    <th className="px-3 py-2 font-mono text-2xs font-medium uppercase tracking-wider text-muted-foreground">
      {children}
    </th>
  )
}
function Td({ children }: { children: React.ReactNode }) {
  return <td className="px-3 py-2 align-middle">{children}</td>
}
