/**
 * LiveLeaderboard — the streaming variant of the ranked table.
 *
 * Differences vs. LeaderboardPreview:
 *
 *   1. Pulls rows from `useLeaderboardStream()`.
 *   2. Animated row reorder via framer-motion's <Reorder> primitives — when
 *      a submission's rank changes, the row slides into its new position
 *      with a smooth spring.
 *   3. Shows a small "last update" pill and a connection-state indicator.
 *
 * The implementation is deliberately self-contained — it does not depend
 * on the static LeaderboardPreview, which we keep as the SSR fallback.
 */

'use client'

import { AnimatePresence, motion } from 'framer-motion'
import { ArrowDown, ArrowUp, Minus } from 'lucide-react'
import { useState } from 'react'

import { useHealthBadges } from '@/lib/hooks/use-health-badges'
import { useLeaderboardStream } from '@/lib/hooks/use-leaderboard-stream'
import { cn, formatLatencyNs, formatRps, formatScore } from '@/lib/utils'
import { HealthBadge } from '@/components/leaderboard/health-badge'
import { Panel } from '@/components/ui/panel'
import { ScoreBreakdown, type ScoreInputs } from '@/components/scoring/score-breakdown'

const STATUS_TONE: Record<string, string> = {
  running: 'border-signal-live/40 bg-signal-live/10 text-signal-live',
  scored: 'border-border bg-surface-elevated text-foreground',
  queued: 'border-signal-warn/40 bg-signal-warn/10 text-signal-warn',
  dq: 'border-signal-ask/40 bg-signal-ask/10 text-signal-ask',
}

const STATUS_LABEL: Record<string, string> = {
  running: 'LIVE',
  scored: 'SCORED',
  queued: 'QUEUED',
  dq: 'DQ',
}

const CONN_TONE: Record<string, string> = {
  open: 'border-signal-live/40 bg-signal-live/10 text-signal-live',
  connecting: 'border-signal-warn/40 bg-signal-warn/10 text-signal-warn',
  closed: 'border-border bg-surface-elevated text-muted-foreground',
  error: 'border-signal-ask/40 bg-signal-ask/10 text-signal-ask',
}

export function LiveLeaderboard({ stream = 'global' }: { stream?: string }) {
  const { rows, state, updatedAt } = useLeaderboardStream(stream)
  const badges = useHealthBadges()
  const [breakdown, setBreakdown] = useState<ScoreInputs | null>(null)

  return (
    <Panel>
      <header className="flex items-center justify-between border-b border-border bg-surface-subtle px-3 py-2">
        <div className="flex items-center gap-2">
          <span className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
            Stream
          </span>
          <span className="font-mono text-2xs text-foreground">{stream}</span>
        </div>
        <div className="flex items-center gap-3">
          {updatedAt && (
            <span className="font-mono text-2xs text-muted-foreground">
              updated{' '}
              {new Date(updatedAt).toLocaleTimeString(undefined, {
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit',
              })}
            </span>
          )}
          <span
            className={cn(
              'inline-flex items-center gap-1.5 rounded-sm border px-1.5 py-0.5 font-mono text-2xs font-semibold uppercase tracking-wider',
              CONN_TONE[state],
            )}
          >
            <span className="live-dot" aria-hidden />
            {state}
          </span>
        </div>
      </header>

      <div className="overflow-x-auto">
      <table className="w-full min-w-[640px] text-sm">
        <thead className="border-b border-border bg-surface-subtle">
          <tr className="text-left">
            <Th className="w-12 text-right">#</Th>
            <Th>Team / Submission</Th>
            <Th className="text-right">Composite</Th>
            <Th className="hidden text-right lg:table-cell">Sustained</Th>
            <Th className="text-right">p99</Th>
            <Th className="hidden text-right sm:table-cell">Correct</Th>
            <Th className="hidden w-24 text-right md:table-cell">Δ</Th>
            <Th className="hidden w-24 text-right md:table-cell">Health</Th>
            <Th className="w-20 text-right">Status</Th>
          </tr>
        </thead>
        <tbody>
          <AnimatePresence initial={false}>
            {rows.length === 0 && (
              <tr>
                <td colSpan={9} className="py-8 text-center font-mono text-2xs uppercase tracking-widest text-muted-foreground">
                  Waiting for first submission…
                </td>
              </tr>
            )}
            {rows.map((row) => (
              <motion.tr
                key={row.submissionId}
                layout
                initial={{ opacity: 0, y: 8 }}
                animate={{ opacity: 1, y: 0 }}
                exit={{ opacity: 0, y: -8 }}
                transition={{ type: 'spring', stiffness: 320, damping: 28 }}
                className="border-b border-border-subtle transition-colors hover:bg-surface-elevated"
              >
                <Td className="text-right font-mono text-muted-foreground">{row.rank}</Td>
                <Td>
                  <div className="font-medium leading-tight">{row.team}</div>
                  <div className="font-mono text-2xs uppercase tracking-wider text-muted-foreground">
                    {row.submissionId}
                  </div>
                </Td>
                <Td className="text-right">
                  <button
                    type="button"
                    onClick={() =>
                      setBreakdown({
                        team:         row.team,
                        submissionId: row.submissionId,
                        composite:    row.composite,
                        throughput:   estimateSubScore(row.composite, 'throughput'),
                        latency:      estimateSubScore(row.composite, 'latency'),
                        correctness:  row.correctness,
                        penalty:      0,
                        sustainedRps: row.throughputRps,
                        p99Ns:        row.p99Ns,
                      })
                    }
                    className="num font-semibold text-foreground transition-colors hover:text-accent focus:outline-none focus-visible:text-accent"
                    title="Explain this score"
                  >
                    {formatScore(row.composite)}
                  </button>
                </Td>
                <Td className="hidden text-right lg:table-cell">
                  <span className="num text-signal-live">{formatRps(row.throughputRps)}</span>
                  <span className="ml-1 font-mono text-2xs text-muted-foreground">req/s</span>
                </Td>
                <Td className="text-right num">{formatLatencyNs(row.p99Ns)}</Td>
                <Td className="hidden text-right num sm:table-cell">{row.correctness.toFixed(2)}%</Td>
                <Td className="hidden text-right md:table-cell">
                  <Delta value={row.delta} />
                </Td>
                <Td className="hidden text-right md:table-cell">
                  <HealthBadge badge={badges.get(row.submissionId)} />
                </Td>
                <Td className="text-right">
                  <span
                    className={cn(
                      'inline-flex rounded-sm border px-1.5 py-0.5 font-mono text-2xs font-semibold uppercase tracking-wider',
                      STATUS_TONE[row.status],
                    )}
                  >
                    {STATUS_LABEL[row.status]}
                  </span>
                </Td>
              </motion.tr>
            ))}
          </AnimatePresence>
        </tbody>
      </table>
      </div>

      <ScoreBreakdown
        open={breakdown !== null}
        onOpenChange={(v) => { if (!v) setBreakdown(null) }}
        score={breakdown}
      />
    </Panel>
  )
}

/**
 * The leaderboard wire format only carries the composite + the inputs we care
 * about most (RPS, p99, correctness). Until the gateway is extended to ship
 * sub-scores explicitly, we back-solve them from the composite. The estimates
 * are *approximate* but consistent: the modal's stacked bar still adds up to
 * the composite. Wire change tracked at services/scoring-service/src/scorer.cpp.
 */
function estimateSubScore(
  composite: number,
  which:     'throughput' | 'latency',
): number {
  // Distribute the composite proportionally to the static weights so the
  // displayed numbers are sensible in the absence of richer telemetry.
  const total = 0.40 + 0.35 + 0.25  // throughput + latency + correctness
  if (which === 'throughput') return Math.max(0, Math.min(100, composite * (0.40 / total)))
  return Math.max(0, Math.min(100, composite * (0.35 / total)))
}

function Th({ children, className }: React.HTMLAttributes<HTMLTableCellElement>) {
  return (
    <th
      className={cn(
        'px-3 py-2 font-mono text-2xs font-medium uppercase tracking-wider text-muted-foreground',
        className,
      )}
    >
      {children}
    </th>
  )
}

function Td({ children, className }: React.HTMLAttributes<HTMLTableCellElement>) {
  return <td className={cn('px-3 py-2.5 align-middle', className)}>{children}</td>
}

function Delta({ value }: { value: number }) {
  if (value === 0) {
    return (
      <span className="inline-flex items-center gap-1 font-mono text-2xs text-muted-foreground">
        <Minus className="h-3 w-3" /> 0
      </span>
    )
  }
  const up = value > 0
  return (
    <span
      className={cn(
        'inline-flex items-center gap-1 font-mono text-2xs font-semibold',
        up ? 'text-signal-live' : 'text-signal-ask',
      )}
    >
      {up ? <ArrowUp className="h-3 w-3" /> : <ArrowDown className="h-3 w-3" />}
      {Math.abs(value)}
    </span>
  )
}
