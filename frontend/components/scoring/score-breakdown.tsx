/**
 * ScoreBreakdown — explain *why* a submission scored what it scored.
 *
 * Opens as a glass dialog (matches the CommandPalette treatment). The user
 * sees the actual scoring formula with values substituted in, a one-line
 * "what it means" beside each sub-score, and a stacked-bar showing each
 * sub-score's contribution to the composite.
 *
 * The formula tracked here MUST stay in lock-step with the C++ scorer at
 * `services/scoring-service/src/scorer.cpp` (functions `throughput_score`,
 * `latency_score`, `composite`). If you change the weights or the latency
 * baseline there, change them here too.
 */

'use client'

import * as Dialog from '@radix-ui/react-dialog'
import { Info, X } from 'lucide-react'
import { useMemo } from 'react'

import { cn, formatLatencyNs, formatRps, formatScore } from '@/lib/utils'

// Kept in sync with services/scoring-service/src/scorer.cpp `composite()`:
//   composite = max(0, 0.40 × t + 0.35 × l + 0.25 × c − p)
// where p is the *unweighted* penalty already capped at 50 by compute_penalty().
export const SCORING_WEIGHTS = {
  throughput:  0.40,
  latency:     0.35,
  correctness: 0.25,
  penalty:     1.00,   // penalty enters with coefficient 1, NOT a small weight
} as const

// Hard cap from compute_penalty(): even with many violations the penalty is
// clamped at 50 so a single bad benchmark cannot zero out an otherwise-strong
// engine. Keep in lock-step with services/scoring-service/src/scorer.cpp.
export const PENALTY_CAP = 50

// p99 baseline (ns) above which the latency score begins to decay. Today this
// is fixed at 30µs in the scorer; surfaced here so the modal renders honestly.
export const LATENCY_BASELINE_NS = 30_000

export type ScoreInputs = {
  team?:        string
  submissionId: string
  composite:    number
  throughput:   number             // sub-score 0..100
  latency:      number             // sub-score 0..100
  correctness:  number             // sub-score 0..100
  penalty:      number             // sub-score 0..100 (subtracted)
  // Underlying measurements — shown alongside each sub-score so the operator
  // can see *what* drove the number, not just the rolled-up value.
  sustainedRps?: number
  targetRps?:    number
  p99Ns?:        number
}

type ScoreBreakdownProps = {
  open:        boolean
  onOpenChange: (v: boolean) => void
  score:       ScoreInputs | null
}

export function ScoreBreakdown({ open, onOpenChange, score }: ScoreBreakdownProps) {
  const rows = useMemo(() => {
    if (!score) return [] as Row[]
    return [
      {
        key:         'throughput',
        label:       'Throughput',
        value:       score.throughput,
        weight:      SCORING_WEIGHTS.throughput,
        tone:        'signal-live',
        meaning:     score.sustainedRps != null && score.targetRps
          ? `sustained ${formatRps(score.sustainedRps)} of ${formatRps(score.targetRps)} target`
          : 'sustained ÷ target RPS, clamped to 100',
        formula:     '100 × min(1, sustained_rps / target_rps)',
      },
      {
        key:         'latency',
        label:       'Latency',
        value:       score.latency,
        weight:      SCORING_WEIGHTS.latency,
        tone:        'accent',
        meaning:     score.p99Ns != null
          ? `p99 = ${formatLatencyNs(score.p99Ns)}, baseline ${formatLatencyNs(LATENCY_BASELINE_NS)}`
          : 'p99 below baseline → 100; decays linearly as p99 grows',
        formula:     'p99 ≤ baseline ? 100 : max(0, 100 − 100 × (p99 − baseline) / baseline)',
      },
      {
        key:         'correctness',
        label:       'Correctness',
        value:       score.correctness,
        weight:      SCORING_WEIGHTS.correctness,
        tone:        'signal-info',
        meaning:     'fills matching the reference orderbook, % of expected',
        formula:     '100 × matched_fills / max(1, expected_fills)',
      },
      {
        key:         'penalty',
        label:       'Penalty',
        value:       score.penalty,
        weight:      -SCORING_WEIGHTS.penalty,
        tone:        'signal-ask',
        meaning:     `5×price + 3×priority + 5×phantom + 2×missing, capped at ${PENALTY_CAP}`,
        formula:     'min(50, 5·price + 3·priority + 5·phantom + 2·missing)',
      },
    ] satisfies Row[]
  }, [score])

  const composite = useMemo(() => {
    if (!score) return 0
    const raw =
      SCORING_WEIGHTS.throughput  * score.throughput  +
      SCORING_WEIGHTS.latency     * score.latency     +
      SCORING_WEIGHTS.correctness * score.correctness -
      SCORING_WEIGHTS.penalty     * Math.min(PENALTY_CAP, score.penalty)
    return Math.max(0, raw)  // matches std::max(0.0, ...) in scorer.cpp
  }, [score])

  return (
    <Dialog.Root open={open} onOpenChange={onOpenChange}>
      <Dialog.Portal>
        <Dialog.Overlay
          className={cn(
            'fixed inset-0 z-50 glass-scrim',
            'data-[state=open]:animate-in data-[state=open]:fade-in-0',
            'data-[state=closed]:animate-out data-[state=closed]:fade-out-0',
            'duration-200',
          )}
        />
        <Dialog.Content
          className={cn(
            'fixed left-1/2 top-1/2 z-[60] w-[44rem] max-w-[94vw] -translate-x-1/2 -translate-y-1/2',
            'rounded-xl overflow-hidden glass-panel',
            'data-[state=open]:animate-in data-[state=open]:fade-in-0 data-[state=open]:zoom-in-95',
            'data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=closed]:zoom-out-95',
            'duration-200 ease-out',
          )}
        >
          <div className="relative z-[1] p-6 space-y-5">
            <header className="flex items-start justify-between">
              <div>
                <Dialog.Title className="font-display text-xl font-semibold tracking-tight">
                  Score breakdown
                </Dialog.Title>
                <Dialog.Description className="text-sm text-muted-foreground">
                  {score?.team
                    ? <>Team <span className="font-mono text-foreground">{score.team}</span> · </>
                    : null}
                  submission <span className="font-mono text-foreground">{score?.submissionId ?? '—'}</span>
                </Dialog.Description>
              </div>
              <Dialog.Close
                className="rounded p-1 text-muted-foreground transition-colors hover:bg-white/[0.06] hover:text-foreground"
                aria-label="Close"
              >
                <X className="h-4 w-4" />
              </Dialog.Close>
            </header>

            {/* Composite badge — the headline number */}
            <div className="flex items-end justify-between rounded-lg border border-white/[0.06] bg-white/[0.03] p-4">
              <div>
                <span className="label-eyebrow">Composite</span>
                <div className="font-mono text-4xl font-semibold tabular-nums text-foreground">
                  {formatScore(composite)}
                </div>
                <p className="mt-0.5 text-xs text-muted-foreground">out of 100</p>
              </div>
              <ContributionBar rows={rows} composite={composite} />
            </div>

            {/* Per-component rows */}
            <div className="space-y-3">
              {rows.map((r) => (
                <Row key={r.key} row={r} />
              ))}
            </div>

            {/* Formula footer — exact composite formula with values substituted in */}
            <footer className="rounded-md border border-white/[0.06] bg-white/[0.02] p-3 font-mono text-2xs leading-relaxed text-muted-foreground">
              <div className="mb-1 flex items-center gap-1.5 text-foreground">
                <Info className="h-3 w-3" />
                composite formula (with values substituted)
              </div>
              <code className="block whitespace-pre">
{score ? `composite = max(0,
            0.40 × ${score.throughput.toFixed(1).padStart(5)}   = ${(0.40 * score.throughput).toFixed(2)}
          + 0.35 × ${score.latency.toFixed(1).padStart(5)}   = ${(0.35 * score.latency).toFixed(2)}
          + 0.25 × ${score.correctness.toFixed(1).padStart(5)}   = ${(0.25 * score.correctness).toFixed(2)}
          −        ${Math.min(PENALTY_CAP, score.penalty).toFixed(1).padStart(5)}   = ${(-Math.min(PENALTY_CAP, score.penalty)).toFixed(2)}
         )      = ${composite.toFixed(2)}` : ''}
              </code>
              <div className="mt-2 text-[10px]">
                Source: <code>services/scoring-service/src/scorer.cpp</code> ·
                weights kept in sync with the C++ scorer; change one, change both.
              </div>
            </footer>
          </div>
        </Dialog.Content>
      </Dialog.Portal>
    </Dialog.Root>
  )
}

/* -------------------------------------------------------------------------- */

type Row = {
  key:     'throughput' | 'latency' | 'correctness' | 'penalty'
  label:   string
  value:   number
  weight:  number  // signed; penalty has weight < 0
  tone:    'signal-live' | 'accent' | 'signal-info' | 'signal-ask'
  meaning: string
  formula: string
}

function Row({ row }: { row: Row }) {
  const contribution = row.value * row.weight
  return (
    <div className="rounded-md border border-white/[0.05] bg-white/[0.02] p-3">
      <div className="flex items-center justify-between">
        <div>
          <div className="flex items-center gap-2">
            <span className="font-semibold">{row.label}</span>
            <span className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
              weight {(Math.abs(row.weight) * 100).toFixed(0)}%
              {row.weight < 0 ? ' (subtracts)' : ''}
            </span>
          </div>
          <p className="mt-0.5 text-xs text-muted-foreground">{row.meaning}</p>
        </div>
        <div className="text-right">
          <div className={cn('font-mono text-lg font-semibold tabular-nums', toneClass(row.tone))}>
            {row.value.toFixed(1)}
          </div>
          <div className="font-mono text-2xs text-muted-foreground">
            contributes {(contribution >= 0 ? '+' : '') + contribution.toFixed(2)}
          </div>
        </div>
      </div>
      {/* Filled bar showing where this sub-score sits in [0, 100] */}
      <div className="mt-3 h-1.5 rounded-full bg-white/[0.04]">
        <div
          className={cn('h-full rounded-full transition-[width]', toneBarClass(row.tone))}
          style={{ width: `${Math.min(100, Math.max(0, row.value))}%` }}
        />
      </div>
      <code className="mt-2 block font-mono text-[10px] text-muted-foreground">
        {row.formula}
      </code>
    </div>
  )
}

function ContributionBar({ rows, composite }: { rows: Row[]; composite: number }) {
  // The bar represents the full 0..100 composite scale. Each positive sub-score
  // is rendered as a coloured segment proportional to its *contribution*
  // (weight × value). The penalty is overlaid as a diagonal-hatch bite on the
  // right edge so the user sees what the penalty cost the score.
  const positives = rows.filter((r) => r.weight > 0)
  const penalty   = rows.find((r) => r.weight < 0)
  const positiveSum = positives.reduce((a, r) => a + r.value * r.weight, 0)
  const penaltyAmt = penalty ? Math.min(PENALTY_CAP, penalty.value) : 0
  return (
    <div className="w-72 space-y-2">
      <div className="flex items-end justify-end gap-1 font-mono text-2xs text-muted-foreground">
        contribution / 100
      </div>
      <div className="relative h-3 w-full overflow-hidden rounded-full border border-white/[0.06] bg-white/[0.03]">
        <div className="flex h-full">
          {positives.map((r) => (
            <div
              key={r.key}
              className={cn('h-full', toneBarClass(r.tone))}
              style={{ width: `${Math.max(0, Math.min(100, r.value * r.weight))}%` }}
              title={`${r.label} +${(r.value * r.weight).toFixed(2)}`}
            />
          ))}
        </div>
        {penaltyAmt > 0 && (
          <div
            className="absolute inset-y-0 right-0 bg-[repeating-linear-gradient(45deg,hsl(var(--signal-ask)/0.6)_0_4px,transparent_4px_8px)]"
            style={{ width: `${Math.min(100, penaltyAmt)}%` }}
            title={`Penalty −${penaltyAmt.toFixed(2)}`}
          />
        )}
      </div>
      <div className="flex items-end justify-end gap-2 font-mono text-2xs">
        <span className="text-muted-foreground">
          positives {positiveSum.toFixed(1)}
        </span>
        <span className="text-foreground">·</span>
        <span className="text-foreground">net {composite.toFixed(1)}</span>
      </div>
    </div>
  )
}

function toneClass(tone: Row['tone']): string {
  switch (tone) {
    case 'signal-live': return 'text-signal-live'
    case 'accent':      return 'text-accent'
    case 'signal-info': return 'text-signal-info'
    case 'signal-ask':  return 'text-signal-ask'
  }
}

function toneBarClass(tone: Row['tone']): string {
  switch (tone) {
    case 'signal-live': return 'bg-signal-live/70'
    case 'accent':      return 'bg-accent/70'
    case 'signal-info': return 'bg-signal-info/70'
    case 'signal-ask':  return 'bg-signal-ask/70'
  }
}
