/**
 * =============================================================================
 *  Velocity — Landing / Overview Dashboard
 *
 *  The first impression. Designed to communicate, at a glance:
 *
 *    1. What the platform does (hero)
 *    2. That it is *live right now* (status panel)
 *    3. The top of the leaderboard (proof)
 *    4. The engineering decisions behind it (cards linking to ADRs)
 *
 *  No fluff, no marketing prose. Every number is real or will become real
 *  the moment a benchmark runs.
 * =============================================================================
 */

import Link from 'next/link'

import { Hero } from '@/components/hero'
import { MetricCard } from '@/components/metric-card'
import { Panel, PanelHeader, PanelTitle, PanelDescription, PanelBody } from '@/components/ui/panel'
import { LeaderboardPreview } from '@/components/leaderboard/leaderboard-preview'
import { MarketdataTicker } from '@/components/marketdata/marketdata-ticker'

export default function HomePage() {
  return (
    <div className="container space-y-12 py-10">
      <Hero />

      {/* ----- Vital signs ------------------------------------------------ */}
      <section aria-labelledby="vital-signs" className="space-y-4">
        <div className="flex items-end justify-between">
          <div>
            <h2 id="vital-signs" className="font-display text-xl font-semibold tracking-tight">
              Platform vital signs
            </h2>
            <p className="text-sm text-muted-foreground">
              Aggregate state of the cluster, refreshed every 250 ms.
            </p>
          </div>
          <span className="label-eyebrow">cluster · velocity-local</span>
        </div>
        <Panel>
          <PanelHeader className="flex items-center justify-between">
            <div>
              <PanelTitle className="text-sm">Reference market data</PanelTitle>
              <PanelDescription className="text-xs">
                OU + jump-shock mid prices, 100 Hz. SPOT / PERP / FUTURES with ρ ≈ 0.95.
              </PanelDescription>
            </div>
          </PanelHeader>
          <PanelBody className="border-t border-border-subtle py-3">
            <MarketdataTicker />
          </PanelBody>
        </Panel>
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-4">
          <MetricCard
            label="Active benchmarks"
            value="0"
            hint="Submissions under load"
            tone="info"
          />
          <MetricCard
            label="Aggregate RPS"
            value="0"
            unit="req/s"
            hint="Sustained across the fleet"
            tone="live"
            trend="flat"
          />
          <MetricCard
            label="Median p99"
            value="—"
            unit="µs"
            hint="Across active submissions"
            tone="live"
            trend="flat"
          />
          <MetricCard
            label="Ingester health"
            value="OK"
            hint="Consumer lag · 0 ms"
            tone="live"
          />
        </div>
      </section>

      {/* ----- Leaderboard preview --------------------------------------- */}
      <section aria-labelledby="leaderboard-preview" className="space-y-4">
        <div className="flex items-end justify-between">
          <div>
            <h2
              id="leaderboard-preview"
              className="font-display text-xl font-semibold tracking-tight"
            >
              Live leaderboard
            </h2>
            <p className="text-sm text-muted-foreground">
              Composite score = 0.40 throughput + 0.35 latency + 0.25 correctness − penalties.
            </p>
          </div>
          <Link
            href="/leaderboard"
            className="text-sm font-medium text-accent hover:underline"
          >
            See all submissions →
          </Link>
        </div>
        <LeaderboardPreview />
      </section>

      {/* ----- Engineering decisions ------------------------------------- */}
      <section aria-labelledby="engineering" className="space-y-4">
        <div>
          <h2 id="engineering" className="font-display text-xl font-semibold tracking-tight">
            How it is built
          </h2>
          <p className="text-sm text-muted-foreground">
            Every defensible choice has a record. Click through for the rationale, the
            alternatives, and the receipts.
          </p>
        </div>
        <div className="grid grid-cols-1 gap-3 md:grid-cols-2 lg:grid-cols-3">
          <DecisionCard
            no="001"
            title="gVisor over Firecracker"
            summary="User-space syscall interception keeps submission code from reaching the host kernel."
            href="https://github.com/narendraxgupta/Velocity/blob/main/docs/adr/001-gvisor-over-firecracker.md"
          />
          <DecisionCard
            no="002"
            title="C++ on the hot path"
            summary="No GC pauses, allocator control, io_uring everywhere it counts."
            href="https://github.com/narendraxgupta/Velocity/blob/main/docs/adr/002-cpp-on-the-hot-path.md"
          />
          <DecisionCard
            no="003"
            title="QuestDB over TimescaleDB"
            summary="ILP TCP writes at multi-million rows/sec — the right choice for telemetry ingest."
            href="https://github.com/narendraxgupta/Velocity/blob/main/docs/adr/003-questdb-over-timescale.md"
          />
          <DecisionCard
            no="004"
            title="Coordinated Omission correction"
            summary="Open-loop load with intended-send-time accounting. Honest tail latency."
            href="https://github.com/narendraxgupta/Velocity/blob/main/docs/adr/004-coordinated-omission.md"
          />
          <DecisionCard
            no="005"
            title="Redpanda over Kafka"
            summary="Kafka wire protocol without the JVM — lower P99, simpler ops."
            href="https://github.com/narendraxgupta/Velocity/blob/main/docs/adr/005-redpanda-over-kafka.md"
          />
          <Panel className="flex flex-col justify-between bg-gradient-radial">
            <PanelBody className="space-y-3 py-5">
              <p className="label-eyebrow">Architecture</p>
              <p className="text-base font-medium leading-snug">
                Read the full architecture blueprint, including data-flow sequence diagrams.
              </p>
            </PanelBody>
            <PanelBody className="border-t border-border-subtle py-3">
              <Link
                href="https://github.com/narendraxgupta/Velocity/blob/main/docs/architecture.md"
                className="text-sm font-medium text-accent hover:underline"
              >
                Open blueprint →
              </Link>
            </PanelBody>
          </Panel>
        </div>
      </section>
    </div>
  )
}

/* -------------------------------------------------------------------------- */

function DecisionCard({
  no,
  title,
  summary,
  href,
}: {
  no: string
  title: string
  summary: string
  href: string
}) {
  return (
    <Panel className="transition-colors hover:border-border-subtle hover:bg-surface-elevated">
      <PanelHeader>
        <span className="label-eyebrow">ADR-{no}</span>
        <PanelTitle className="mt-1 text-base">{title}</PanelTitle>
      </PanelHeader>
      <PanelBody className="pt-0">
        <PanelDescription>{summary}</PanelDescription>
      </PanelBody>
      <PanelBody className="border-t border-border-subtle py-3">
        <Link href={href} className="text-sm font-medium text-accent hover:underline">
          Read the record →
        </Link>
      </PanelBody>
    </Panel>
  )
}
