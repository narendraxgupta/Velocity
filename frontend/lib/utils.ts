/**
 * Lightweight utility helpers used across the UI.
 *
 * `cn` — composes Tailwind class strings with conflict-resolution. This is
 * the canonical shadcn/ui pattern.
 *
 * The number formatters are deliberately kept here (rather than in some
 * "i18n" abstraction) because Velocity displays numbers in only one locale:
 * the engineer's. We use grouping for readability, the IEC binary prefixes
 * for memory, and SI prefixes for rates.
 */

import { type ClassValue, clsx } from 'clsx'
import { twMerge } from 'tailwind-merge'

export function cn(...inputs: ClassValue[]): string {
  return twMerge(clsx(inputs))
}

/* -------------------------------------------------------------------------- */
/* Number formatting                                                          */
/* -------------------------------------------------------------------------- */

/**
 * Format a request-per-second number as a human-friendly SI value.
 *
 *   42         -> "42"
 *   4_200      -> "4.2k"
 *   1_234_567  -> "1.23M"
 *   12_345_678 -> "12.3M"
 */
export function formatRps(value: number): string {
  if (!Number.isFinite(value)) return '—'
  const abs = Math.abs(value)
  if (abs < 1_000) return value.toFixed(0)
  if (abs < 1_000_000) return `${(value / 1_000).toFixed(value < 10_000 ? 2 : 1)}k`
  if (abs < 1_000_000_000) return `${(value / 1_000_000).toFixed(value < 10_000_000 ? 2 : 1)}M`
  return `${(value / 1_000_000_000).toFixed(2)}B`
}

/**
 * Format a nanosecond latency as the most readable time unit.
 *
 *   850       ns  -> "850ns"
 *   1_234     ns  -> "1.23µs"
 *   12_345    ns  -> "12.3µs"
 *   1_234_567 ns  -> "1.23ms"
 */
export function formatLatencyNs(nanos: number): string {
  if (!Number.isFinite(nanos)) return '—'
  if (nanos < 1_000) return `${nanos.toFixed(0)}ns`
  if (nanos < 1_000_000) {
    const us = nanos / 1_000
    return `${us.toFixed(us < 10 ? 2 : us < 100 ? 1 : 0)}µs`
  }
  if (nanos < 1_000_000_000) {
    const ms = nanos / 1_000_000
    return `${ms.toFixed(ms < 10 ? 2 : 1)}ms`
  }
  return `${(nanos / 1_000_000_000).toFixed(2)}s`
}

/**
 * Format a score in the range [0, 100] to two decimal places, with a sign
 * for ranking deltas.
 */
export function formatScore(score: number, opts?: { signed?: boolean }): string {
  if (!Number.isFinite(score)) return '—'
  const formatted = score.toFixed(2)
  if (opts?.signed && score > 0) return `+${formatted}`
  return formatted
}

/**
 * Compact relative-time string: "now", "2s ago", "1m ago", "1h ago".
 *
 * Intentionally fuzzy beyond a minute — precision past that point is rarely
 * needed in a live dashboard.
 */
export function formatRelativeMs(ms: number): string {
  const seconds = Math.round(ms / 1000)
  if (seconds < 1) return 'now'
  if (seconds < 60) return `${seconds}s ago`
  const minutes = Math.round(seconds / 60)
  if (minutes < 60) return `${minutes}m ago`
  const hours = Math.round(minutes / 60)
  if (hours < 24) return `${hours}h ago`
  return `${Math.round(hours / 24)}d ago`
}

/**
 * Truncate a string in the middle with an ellipsis. Useful for sub-IDs,
 * hashes, container ids — long enough to be unique, short enough to fit.
 *
 *   middleTruncate("01HQ8KZRJ2P0Y4VBV7M6E9XQGH", 12) -> "01HQ8K…E9XQGH"
 */
export function middleTruncate(value: string, max: number): string {
  if (value.length <= max) return value
  const keep = Math.floor((max - 1) / 2)
  return `${value.slice(0, keep)}…${value.slice(value.length - keep)}`
}
