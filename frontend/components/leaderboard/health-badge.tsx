/**
 * HealthBadge — tiny pill rendered next to a leaderboard row showing the
 * Isolation Forest verdict for that submission.
 *
 * Tone mapping
 *   ok       → muted: nothing to worry about, ground state
 *   watch    → warn:  bottom 10% of the recent window
 *   anomaly  → ask:   bottom 2% — operator should investigate
 *
 * Reason text appears in the title attribute so it doesn't dominate
 * the row layout.
 */

'use client'

import { cn } from '@/lib/utils'

import type { HealthBadge as HealthBadgeData } from '@/lib/hooks/use-health-badges'

const TONE: Record<HealthBadgeData['health'], string> = {
  ok:      'border-border bg-surface-elevated text-muted-foreground',
  watch:   'border-signal-warn/40 bg-signal-warn/10 text-signal-warn',
  anomaly: 'border-signal-ask/40 bg-signal-ask/10 text-signal-ask',
}

const LABEL: Record<HealthBadgeData['health'], string> = {
  ok:      'OK',
  watch:   'WATCH',
  anomaly: 'ANOMALY',
}

export function HealthBadge({ badge }: { badge?: HealthBadgeData }) {
  if (!badge) return null
  return (
    <span
      title={badge.reason}
      className={cn(
        'inline-flex items-center gap-1 rounded-sm border px-1.5 py-0.5 font-mono text-2xs font-semibold uppercase tracking-wider',
        TONE[badge.health],
      )}
    >
      <span aria-hidden>{badge.health === 'anomaly' ? '!' : badge.health === 'watch' ? '~' : '\u00B7'}</span>
      {LABEL[badge.health]}
      <span className="ml-1 font-mono text-2xs opacity-70">
        {badge.rank_pct.toFixed(0)}%
      </span>
    </span>
  )
}
