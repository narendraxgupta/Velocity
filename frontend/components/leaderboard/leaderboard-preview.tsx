/**
 * LeaderboardPreview — the dense, terminal-style top-N table shown on the
 * landing page. Real data flows in from the Leaderboard WebSocket once a
 * benchmark runs; for the empty state we render a static, beautifully laid
 * out placeholder that already looks "real."
 *
 * The component is intentionally server-rendered. The live, animated
 * variant lives at `/leaderboard` and pulls from the streaming hook.
 */

import { ArrowDown, ArrowUp, Minus } from 'lucide-react'

import { Panel } from '@/components/ui/panel'
import { cn, formatRps, formatLatencyNs, formatScore } from '@/lib/utils'

/* -------------------------------------------------------------------------- */
/* Demo data — replaced by streaming snapshot when the WS connects.            */
/* -------------------------------------------------------------------------- */

type PreviewRow = {
  rank: number
  team: string
  submission: string
  composite: number
  throughputRps: number
  p99Ns: number
  correctness: number
  status: 'running' | 'scored' | 'pending' | 'disqualified'
  delta: number
}

const placeholderRows: PreviewRow[] = [
  // Empty array → renders the empty state with the call-to-action.
]

const statusTone: Record<PreviewRow['status'], string> = {
  running: 'border-signal-live/40 bg-signal-live/10 text-signal-live',
  scored: 'border-border bg-surface-elevated text-foreground',
  pending: 'border-signal-warn/40 bg-signal-warn/10 text-signal-warn',
  disqualified: 'border-signal-ask/40 bg-signal-ask/10 text-signal-ask',
}

const statusLabel: Record<PreviewRow['status'], string> = {
  running: 'LIVE',
  scored: 'SCORED',
  pending: 'QUEUED',
  disqualified: 'DQ',
}

/* -------------------------------------------------------------------------- */

export function LeaderboardPreview() {
  if (placeholderRows.length === 0) {
    return <EmptyLeaderboard />
  }

  return (
    <Panel>
      <table className="w-full text-sm">
        <thead className="border-b border-border bg-surface-subtle">
          <tr className="text-left">
            <Th className="w-12 text-right">#</Th>
            <Th>Team / Submission</Th>
            <Th className="text-right">Composite</Th>
            <Th className="text-right">Sustained</Th>
            <Th className="text-right">p99</Th>
            <Th className="text-right">Correct</Th>
            <Th className="w-24 text-right">Δ</Th>
            <Th className="w-20 text-right">Status</Th>
          </tr>
        </thead>
        <tbody>
          {placeholderRows.map((row) => (
            <tr
              key={row.submission}
              className="border-b border-border-subtle transition-colors hover:bg-surface-elevated"
            >
              <Td className="text-right font-mono text-muted-foreground">{row.rank}</Td>
              <Td>
                <div className="font-medium leading-tight">{row.team}</div>
                <div className="font-mono text-2xs uppercase tracking-wider text-muted-foreground">
                  {row.submission}
                </div>
              </Td>
              <Td className="text-right">
                <span className="num font-semibold text-foreground">
                  {formatScore(row.composite)}
                </span>
              </Td>
              <Td className="text-right">
                <span className="num text-signal-live">{formatRps(row.throughputRps)}</span>
                <span className="ml-1 font-mono text-2xs text-muted-foreground">req/s</span>
              </Td>
              <Td className="text-right num">{formatLatencyNs(row.p99Ns)}</Td>
              <Td className="text-right num">{row.correctness.toFixed(2)}%</Td>
              <Td className="text-right">
                <Delta value={row.delta} />
              </Td>
              <Td className="text-right">
                <span
                  className={cn(
                    'inline-flex rounded-sm border px-1.5 py-0.5 font-mono text-2xs font-semibold uppercase tracking-wider',
                    statusTone[row.status],
                  )}
                >
                  {statusLabel[row.status]}
                </span>
              </Td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  )
}

/* -------------------------------------------------------------------------- */

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

function EmptyLeaderboard() {
  return (
    <Panel className="grid place-items-center bg-grid bg-[length:32px_32px]">
      <div className="flex flex-col items-center gap-3 py-16 text-center">
        <div className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
          Awaiting first submission
        </div>
        <p className="max-w-md text-base text-muted-foreground">
          The leaderboard activates as soon as the first benchmark begins. Submit a matching
          engine to see your score appear here within seconds.
        </p>
        <div className="mt-2 inline-flex items-center gap-1.5 rounded-md border border-border-subtle bg-surface px-3 py-1.5 font-mono text-2xs uppercase tracking-wider text-muted-foreground">
          <span className="live-dot" aria-hidden />
          Listening on <span className="text-foreground">ws://leaderboard-ws/ws</span>
        </div>
      </div>
    </Panel>
  )
}
