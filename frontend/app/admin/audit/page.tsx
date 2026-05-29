/**
 * /admin/audit — append-only audit log viewer.
 *
 * Admin-only. The page renders empty for non-admin viewers (we let the
 * RoleGate keep the surface honest); the gateway also returns 403 for
 * the underlying /v1/audit endpoint, so even a forged client gets
 * nothing useful.
 *
 * Pagination is "cursor by occurred_at". The audit log is
 * time-series-shaped so descending-time + LIMIT is the right shape;
 * we don't bother with offset pagination.
 */

'use client'

import { useCallback, useEffect, useMemo, useState } from 'react'

import { RoleGate } from '@/components/auth/role-gate'
import { EmptyState } from '@/components/ui/empty-state'
import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'
import { apiJson } from '@/lib/api/client'
import { cn, formatRelativeMs, middleTruncate } from '@/lib/utils'

type AuditEvent = {
  event_id: string
  tenant_id: string
  subject: string
  role: string
  source: string
  action: string
  resource_type: string
  resource_id: string
  outcome: 'allow' | 'deny' | 'error'
  status_code: number
  occurred_at_ns: number
  remote_ip: string
  request_id: string
  meta?: Record<string, unknown>
  chain_hash?: string
}

type Filters = {
  tenant: string
  action: string
  since: string
  limit: number
}

const DEFAULT_FILTERS: Filters = {
  tenant: '',
  action: '',
  since: '',
  limit: 200,
}

export default function AuditPage() {
  return (
    <RoleGate
      cap="audit:read"
      fallback={
        <div className="container py-12">
          <EmptyState
            title="Admin-only"
            description="The audit log is restricted to platform admins."
          />
        </div>
      }
    >
      <AuditView />
    </RoleGate>
  )
}

function AuditView() {
  const [filters, setFilters] = useState<Filters>(DEFAULT_FILTERS)
  const [events, setEvents] = useState<AuditEvent[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)

  const query = useCallback(async () => {
    setLoading(true)
    setError(null)
    try {
      const qs = new URLSearchParams()
      if (filters.tenant) qs.set('tenant', filters.tenant)
      if (filters.action) qs.set('action', filters.action)
      if (filters.since)  qs.set('since',  filters.since)
      qs.set('limit', String(filters.limit))
      const body = await apiJson<{ events: AuditEvent[]; count: number }>(
        `/v1/audit?${qs.toString()}`,
      )
      setEvents(body.events ?? [])
    } catch (e) {
      setError(String(e))
      setEvents([])
    } finally {
      setLoading(false)
    }
  }, [filters])

  useEffect(() => { void query() }, [query])

  const grouped = useMemo(() => {
    const m = new Map<string, AuditEvent[]>()
    for (const e of events) {
      const day = new Date(e.occurred_at_ns / 1e6).toISOString().slice(0, 10)
      const arr = m.get(day) ?? []
      arr.push(e)
      m.set(day, arr)
    }
    return [...m.entries()].sort(([a], [b]) => b.localeCompare(a))
  }, [events])

  return (
    <div className="container space-y-6 py-8">
      <header>
        <span className="label-eyebrow">platform admin</span>
        <h1 className="font-display text-2xl font-semibold tracking-tight">
          Audit log
        </h1>
        <p className="text-sm text-muted-foreground">
          Hash-chained, append-only record of every privileged action across
          the platform. Stored in QuestDB; verify the chain off-cluster with{' '}
          <code className="font-mono">scripts/audit-verify</code>.
        </p>
      </header>

      <Panel>
        <PanelHeader>
          <PanelTitle>Filter</PanelTitle>
          <PanelDescription>
            All filters are SQL-bound parameters on the audit-log service.
            Leave blank to match everything.
          </PanelDescription>
        </PanelHeader>
        <PanelBody className="grid grid-cols-1 gap-3 sm:grid-cols-4">
          <FilterInput
            label="Tenant"
            value={filters.tenant}
            onChange={(v) => setFilters({ ...filters, tenant: v })}
            placeholder="acme"
          />
          <FilterInput
            label="Action"
            value={filters.action}
            onChange={(v) => setFilters({ ...filters, action: v })}
            placeholder="submission.create"
          />
          <FilterInput
            label="Since (RFC3339)"
            value={filters.since}
            onChange={(v) => setFilters({ ...filters, since: v })}
            placeholder="2026-05-19T00:00:00Z"
          />
          <FilterInput
            label="Limit"
            value={String(filters.limit)}
            onChange={(v) => setFilters({ ...filters, limit: Math.max(1, Math.min(5000, Number(v) || 200)) })}
            placeholder="200"
          />
        </PanelBody>
      </Panel>

      {error && (
        <div className="rounded-md border border-signal-warn/30 bg-signal-warn/10 px-3 py-2 font-mono text-xs text-signal-warn">
          {error}
        </div>
      )}

      {loading && events.length === 0 ? (
        <EmptyState title="Loading…" description="Querying audit-log service." />
      ) : events.length === 0 ? (
        <EmptyState
          title="No events"
          description="No audit events match the current filter. Adjust the date range or clear filters."
        />
      ) : (
        <div className="space-y-6">
          {grouped.map(([day, dayEvents]) => (
            <section key={day} className="space-y-2">
              <h2 className="label-eyebrow text-foreground">{day}</h2>
              <div className="overflow-x-auto rounded-md border border-border bg-surface">
                <table className="w-full text-left text-sm">
                  <thead className="bg-background text-2xs uppercase tracking-widest text-muted-foreground">
                    <tr>
                      <th className="px-3 py-2">When</th>
                      <th className="px-3 py-2">Tenant</th>
                      <th className="px-3 py-2">Subject</th>
                      <th className="px-3 py-2">Action</th>
                      <th className="px-3 py-2">Resource</th>
                      <th className="px-3 py-2">Outcome</th>
                      <th className="px-3 py-2">Status</th>
                      <th className="px-3 py-2">IP</th>
                    </tr>
                  </thead>
                  <tbody>
                    {dayEvents.map((e) => (
                      <tr key={e.event_id} className="border-t border-border-subtle">
                        <td className="px-3 py-2 font-mono text-2xs">
                          {formatRelativeMs(Date.now() - e.occurred_at_ns / 1e6)}
                        </td>
                        <td className="px-3 py-2 font-mono text-2xs">{e.tenant_id}</td>
                        <td className="px-3 py-2 font-mono text-2xs">
                          {middleTruncate(e.subject || '—', 16)}
                        </td>
                        <td className="px-3 py-2 font-mono text-xs">{e.action}</td>
                        <td className="px-3 py-2 font-mono text-2xs">
                          {e.resource_type}/{middleTruncate(e.resource_id, 14)}
                        </td>
                        <td className="px-3 py-2">
                          <span className={cn(
                            'rounded border px-1.5 py-0.5 font-mono text-2xs font-semibold uppercase tracking-widest',
                            e.outcome === 'allow'
                              ? 'border-accent/40 text-accent'
                              : e.outcome === 'deny'
                                ? 'border-signal-warn/40 text-signal-warn'
                                : 'border-destructive/40 text-destructive',
                          )}>
                            {e.outcome}
                          </span>
                        </td>
                        <td className="px-3 py-2 font-mono text-2xs">{e.status_code}</td>
                        <td className="px-3 py-2 font-mono text-2xs text-muted-foreground">{e.remote_ip}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </section>
          ))}
        </div>
      )}
    </div>
  )
}

function FilterInput({
  label, value, onChange, placeholder,
}: { label: string; value: string; onChange: (v: string) => void; placeholder?: string }) {
  return (
    <label className="block space-y-1">
      <span className="label-eyebrow">{label}</span>
      <input
        value={value}
        onChange={(e) => onChange(e.target.value)}
        placeholder={placeholder}
        className="w-full rounded border border-border bg-surface px-3 py-2 font-mono text-sm outline-none focus:border-accent focus:ring-1 focus:ring-accent"
      />
    </label>
  )
}
