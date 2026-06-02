/**
 * /share/<token> — public, unauthenticated snapshot of a benchmark run.
 *
 * The page deliberately:
 *   - Does NOT call useSession() — share viewers may have no account.
 *   - Does NOT load the SDK's app-shell chrome (nav, RoleGate, etc.) —
 *     the viewer should see ONLY the run summary, branded as Velocity.
 *   - Uses ISR (revalidate=3600) to keep cold loads snappy. Snapshots
 *     are immutable so this is safe; the gateway sets the same
 *     Cache-Control header.
 */

import type { Metadata } from 'next'

import { formatLatencyNs, formatRps, formatScore } from '@/lib/utils'

export const revalidate = 3600

// Server-component fetches happen INSIDE the Next pod, which usually
// can't reach the public ingress hostname (it'd hairpin through a
// load balancer for no reason, or fail entirely in air-gapped
// clusters). Prefer the cluster-internal URL when present and fall
// back to the public one for local dev where they point at the same
// localhost:8080.
const API_BASE =
  process.env.INTERNAL_API_GATEWAY_URL ??
  process.env.NEXT_PUBLIC_API_GATEWAY_URL ??
  'http://localhost:8080'

type SharePayload = {
  team: string
  display: string
  profile: string
  composite_score: number
  latency_ns: {
    p50: number
    p90: number
    p99: number
    p999: number
    max: number
  }
  throughput_rps: number
  rank_at_mint: number
  snapshot_at_ns: number
}

export async function generateMetadata(
  { params }: { params: Promise<{ token: string }> },
): Promise<Metadata> {
  const { token } = await params
  const res = await fetch(`${API_BASE}/v1/share/${encodeURIComponent(token)}`, {
    next: { revalidate },
  })
  if (!res.ok) return { title: 'Velocity — Share' }
  const data = (await res.json()) as SharePayload
  return {
    title: `${data.team} · ${data.display} · Velocity`,
    description: `Benchmark profile=${data.profile}, score=${formatScore(data.composite_score)}, p99=${formatLatencyNs(data.latency_ns.p99)}.`,
    openGraph: {
      title: `${data.team} · ${data.display}`,
      description: `Velocity benchmark run · score=${formatScore(data.composite_score)} · p99=${formatLatencyNs(data.latency_ns.p99)}`,
    },
  }
}

export default async function SharePage(
  { params }: { params: Promise<{ token: string }> },
) {
  const { token } = await params
  const res = await fetch(`${API_BASE}/v1/share/${encodeURIComponent(token)}`, {
    next: { revalidate },
  })

  if (!res.ok) {
    return (
      <div className="mx-auto max-w-2xl px-6 py-24 text-center">
        <h1 className="font-display text-3xl font-semibold">Share not found</h1>
        <p className="mt-2 text-sm text-muted-foreground">
          This link is invalid or has expired. Ask the team for a fresh one.
        </p>
      </div>
    )
  }

  const data = (await res.json()) as SharePayload
  const snapshotAt = new Date(data.snapshot_at_ns / 1e6)

  return (
    <div className="mx-auto max-w-3xl px-6 py-16">
      <header className="space-y-1">
        <span className="label-eyebrow">benchmark share</span>
        <h1 className="font-display text-3xl font-semibold tracking-tight">
          {data.team} · {data.display}
        </h1>
        <p className="text-sm text-muted-foreground">
          {data.profile ? `${data.profile} profile · ` : ''}snapshotted {snapshotAt.toUTCString()}
        </p>
      </header>

      <section className="mt-10 grid grid-cols-2 gap-3 sm:grid-cols-4">
        <Stat label="Composite score" value={formatScore(data.composite_score)} highlight />
        <Stat label="Rank at mint" value={`#${data.rank_at_mint || '—'}`} />
        <Stat label="Throughput" value={formatRps(data.throughput_rps)} hint="rps" />
        <Stat label="p99 latency"  value={formatLatencyNs(data.latency_ns.p99)} />
      </section>

      <section className="mt-10 rounded-md border border-border bg-surface p-6">
        <h2 className="label-eyebrow text-foreground">Latency profile</h2>
        <div className="mt-3 grid grid-cols-2 gap-3 font-mono text-sm sm:grid-cols-5">
          <LatencyRow label="p50"  value={data.latency_ns.p50} />
          <LatencyRow label="p90"  value={data.latency_ns.p90} />
          <LatencyRow label="p99"  value={data.latency_ns.p99} />
          <LatencyRow label="p999" value={data.latency_ns.p999} />
          <LatencyRow label="max"  value={data.latency_ns.max} />
        </div>
      </section>

      <footer className="mt-16 border-t border-border-subtle pt-6 text-center text-xs text-muted-foreground">
        Powered by{' '}
        <a href="/" className="font-medium text-accent hover:underline">
          Velocity
        </a>
        {' '}— the benchmarking platform for low-latency trading systems.
      </footer>
    </div>
  )
}

function Stat({
  label, value, hint, highlight,
}: { label: string; value: string; hint?: string; highlight?: boolean }) {
  return (
    <div className={`rounded-md border ${highlight ? 'border-accent/40 bg-accent/10' : 'border-border bg-surface'} px-4 py-3`}>
      <div className="label-eyebrow">{label}</div>
      <div className={`mt-1 font-display text-2xl font-semibold ${highlight ? 'text-accent' : ''}`}>
        {value}
        {hint && <span className="ml-1 text-2xs font-mono uppercase text-muted-foreground">{hint}</span>}
      </div>
    </div>
  )
}

function LatencyRow({ label, value }: { label: string; value: number }) {
  return (
    <div>
      <div className="label-eyebrow">{label}</div>
      <div className="mt-1 font-mono text-base">{formatLatencyNs(value)}</div>
    </div>
  )
}
