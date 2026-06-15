/**
 * MetricCard — the small "single number with a label" tile used on dashboards.
 *
 * Designed to communicate state in milliseconds of glance time:
 *   - Eyebrow label (the "what")
 *   - Big number with unit (the "how much")
 *   - Subtle hint underneath (the "context")
 *   - Optional trend arrow + tone (the "is this good or bad")
 *
 * No icons by default — icons compete with numbers for attention.
 */

import { cn } from '@/lib/utils'

type Tone = 'live' | 'ask' | 'info' | 'warn' | 'accent' | 'neutral'
type Trend = 'up' | 'down' | 'flat'

export interface MetricCardProps {
  label: string
  value: string | number
  unit?: string
  hint?: string
  tone?: Tone
  trend?: Trend
  /** Optional bottom-right badge — e.g., a delta or a status word. */
  badge?: string
  /** Inline subdued ASCII sparkline drawn in mono — set via SSR for now. */
  sparkline?: string
  className?: string
}

const toneText: Record<Tone, string> = {
  live: 'text-signal-live',
  ask: 'text-signal-ask',
  info: 'text-signal-info',
  warn: 'text-signal-warn',
  accent: 'text-accent',
  neutral: 'text-foreground',
}

const toneRing: Record<Tone, string> = {
  live: 'before:bg-signal-live',
  ask: 'before:bg-signal-ask',
  info: 'before:bg-signal-info',
  warn: 'before:bg-signal-warn',
  accent: 'before:bg-accent',
  neutral: 'before:bg-muted',
}

export function MetricCard({
  label,
  value,
  unit,
  hint,
  tone = 'neutral',
  trend,
  badge,
  sparkline,
  className,
}: MetricCardProps) {
  return (
    <div
      className={cn(
        'group relative isolate flex flex-col gap-2 overflow-hidden rounded-md border border-border bg-surface px-4 py-3 shadow-panel-sm',
        'transition-colors duration-200 hover:border-border-subtle hover:bg-surface-elevated',
        'before:absolute before:left-0 before:top-0 before:h-full before:w-[2px] before:opacity-70 before:transition-[width,opacity] group-hover:before:w-[3px] group-hover:before:opacity-100',
        toneRing[tone],
        className,
      )}
    >
      <div className="flex items-center justify-between">
        <span className="label-eyebrow">{label}</span>
        {trend && <TrendGlyph dir={trend} className={toneText[tone]} />}
      </div>

      <div className="flex items-baseline gap-1.5">
        <span className={cn('num font-display text-2xl font-semibold', toneText[tone])}>
          {value}
        </span>
        {unit && (
          <span className="font-mono text-xs font-medium uppercase tracking-wider text-muted-foreground">
            {unit}
          </span>
        )}
      </div>

      {sparkline && (
        <pre
          aria-hidden
          className="overflow-hidden font-mono text-[10px] leading-none text-muted-foreground/70"
        >
          {sparkline}
        </pre>
      )}

      {(hint || badge) && (
        <div className="mt-auto flex items-center justify-between text-2xs">
          {hint && <span className="font-mono uppercase tracking-wider text-muted-foreground">{hint}</span>}
          {badge && (
            <span className="rounded-sm border border-border-subtle bg-surface-subtle px-1.5 py-0.5 font-mono tracking-wide text-muted-foreground">
              {badge}
            </span>
          )}
        </div>
      )}
    </div>
  )
}

function TrendGlyph({ dir, className }: { dir: Trend; className?: string }) {
  const glyph = dir === 'up' ? '▲' : dir === 'down' ? '▼' : '◆'
  return (
    <span aria-hidden className={cn('text-xs', className)}>
      {glyph}
    </span>
  )
}
