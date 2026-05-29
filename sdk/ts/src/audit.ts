/**
 * Audit log resource (`/v1/audit`).
 *
 * Admin-only on the gateway side; calls will 403 unless the caller's
 * JWT carries `role=admin`. Filter on tenant/action/time-range; the
 * server enforces a 5 000-row hard cap regardless of the requested
 * limit.
 */

import type { VelocityClient } from './client.js'

export type AuditEvent = {
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
  chain_hash?: string
  meta?: Record<string, unknown>
}

export type AuditQuery = {
  tenant?: string
  action?: string
  since?: Date
  until?: Date
  limit?: number
}

export class AuditResource {
  constructor(private readonly client: VelocityClient) {}

  async query(q: AuditQuery = {}): Promise<AuditEvent[]> {
    const params = new URLSearchParams()
    if (q.tenant) params.set('tenant', q.tenant)
    if (q.action) params.set('action', q.action)
    if (q.since)  params.set('since',  q.since.toISOString())
    if (q.until)  params.set('until',  q.until.toISOString())
    if (q.limit)  params.set('limit',  String(q.limit))
    const resp = await this.client.request<{ events: AuditEvent[]; count: number }>(
      'GET', `/v1/audit?${params.toString()}`,
    )
    return resp.events
  }
}
