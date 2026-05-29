/**
 * Leaderboard page — full ranked list, plus a tab for the underlying telemetry.
 *
 * The current implementation renders an empty-state with the same dense
 * table format that the live variant will use. Once `useLeaderboardStream()`
 * (forthcoming) is wired in, this becomes the real-time view.
 */

import type { Metadata } from 'next'

import { LiveLeaderboard } from '@/components/leaderboard/live-leaderboard'
import { Panel, PanelHeader, PanelTitle, PanelDescription } from '@/components/ui/panel'

export const metadata: Metadata = {
  title: 'Leaderboard',
}

export default function LeaderboardPage() {
  return (
    <div className="container space-y-6 py-8">
      <header className="flex items-end justify-between">
        <div>
          <span className="label-eyebrow">Live ranking · all submissions</span>
          <h1 className="font-display text-2xl font-semibold tracking-tight">Leaderboard</h1>
          <p className="text-sm text-muted-foreground">
            Ranked by composite score across the four canonical profiles.
          </p>
        </div>
        <span className="inline-flex items-center gap-1.5 rounded-md border border-signal-live/40 bg-signal-live/10 px-2.5 py-1 font-mono text-2xs font-semibold uppercase tracking-widest text-signal-live">
          <span className="live-dot" aria-hidden />
          Streaming
        </span>
      </header>

      <LiveLeaderboard stream="global" />

      <Panel>
        <PanelHeader>
          <PanelTitle>Scoring methodology</PanelTitle>
          <PanelDescription>
            <span className="font-mono">
              S = 0.40 × throughput + 0.35 × latency + 0.25 × correctness − penalties
            </span>
            . p99 is computed from a merged HdrHistogram across all workers; sustained RPS is
            the p10 of 1-second TPS samples (not the mean, because the worst sustained number is
            what matters).
          </PanelDescription>
        </PanelHeader>
      </Panel>
    </div>
  )
}
