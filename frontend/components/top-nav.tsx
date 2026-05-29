/**
 * Top navigation — slim, dark, Bloomberg-density. The brand wordmark sits on
 * the left; primary destinations follow; environment status and the LIVE
 * indicator sit on the right.
 *
 * Rendered server-side. The LIVE indicator's pulse is pure CSS so it works
 * without hydration. The route highlight uses pathname matching client-side.
 */

'use client'

import Link from 'next/link'
import { usePathname } from 'next/navigation'

import { ThemeToggle } from '@/components/ui/theme-toggle'
import { cn } from '@/lib/utils'

const nav = [
  { href: '/', label: 'Overview' },
  { href: '/leaderboard', label: 'Leaderboard' },
  { href: '/submissions', label: 'Submissions' },
  { href: '/fleet', label: 'Fleet' },
  { href: '/compare', label: 'Compare' },
  { href: '/admin', label: 'Admin' },
] as const

export function TopNav() {
  const pathname = usePathname()

  return (
    <header className="sticky top-0 z-50 h-[var(--header-h)] border-b border-border bg-background/85 backdrop-blur-md backdrop-saturate-150">
      <div className="container flex h-full items-center justify-between gap-6">
        {/* Brand */}
        <Link
          href="/"
          className="flex items-center gap-2.5 transition-opacity hover:opacity-80"
          aria-label="Velocity home"
        >
          <BrandMark className="h-5 w-5" />
          <span className="font-display text-base font-semibold tracking-tight">
            Velocity
          </span>
          <span className="hidden font-mono text-2xs uppercase tracking-widest text-muted-foreground sm:inline">
            / benchmark
          </span>
        </Link>

        {/* Primary nav */}
        <nav className="flex items-center gap-1" aria-label="Primary">
          {nav.map((item) => {
            const active =
              item.href === '/'
                ? pathname === '/'
                : pathname.startsWith(item.href)
            return (
              <Link
                key={item.href}
                href={item.href}
                className={cn(
                  'rounded px-3 py-1.5 text-sm font-medium transition-colors',
                  active
                    ? 'bg-surface-elevated text-foreground shadow-inset-border'
                    : 'text-muted-foreground hover:bg-surface hover:text-foreground',
                )}
              >
                {item.label}
              </Link>
            )
          })}
        </nav>

        {/* Status cluster */}
        <div className="flex items-center gap-3">
          <KbdHint />
          <EnvBadge />
          <ThemeToggle />
          <LiveBadge />
        </div>
      </div>
    </header>
  )
}

/* -------------------------------------------------------------------------- */

function BrandMark({ className }: { className?: string }) {
  return (
    <svg
      viewBox="0 0 24 24"
      fill="none"
      className={className}
      role="img"
      aria-label="Velocity logo"
    >
      <defs>
        <linearGradient id="velocity-grad" x1="0" y1="0" x2="24" y2="24" gradientUnits="userSpaceOnUse">
          <stop offset="0%"  stopColor="hsl(155 100% 50%)" />
          <stop offset="60%" stopColor="hsl(189  90% 58%)" />
          <stop offset="100%" stopColor="hsl(251 100% 70%)" />
        </linearGradient>
      </defs>
      {/* Stylized "V" with motion lines */}
      <path
        d="M3 4l7 14L17 4"
        stroke="url(#velocity-grad)"
        strokeWidth="2.25"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
      <path d="M14 11h6"  stroke="url(#velocity-grad)" strokeWidth="2" strokeLinecap="round" opacity="0.7" />
      <path d="M17 16h4"  stroke="url(#velocity-grad)" strokeWidth="2" strokeLinecap="round" opacity="0.4" />
    </svg>
  )
}

function EnvBadge() {
  // Public env var so it's available client-side without hydration weirdness.
  const env = process.env.NEXT_PUBLIC_VELOCITY_ENV ?? 'dev'
  return (
    <span
      className="hidden items-center gap-1.5 rounded border border-border-subtle bg-surface px-2 py-1 font-mono text-2xs uppercase tracking-wider text-muted-foreground md:inline-flex"
      title="Deployment environment"
    >
      <span className="h-1 w-1 rounded-full bg-signal-info" aria-hidden />
      env.{env}
    </span>
  )
}

function KbdHint() {
  return (
    <span
      className="hidden items-center gap-1.5 rounded border border-border-subtle bg-surface px-2 py-1 font-mono text-2xs uppercase tracking-wider text-muted-foreground lg:inline-flex"
      title="Open the command palette"
    >
      <kbd>Ctrl</kbd>
      <kbd>K</kbd>
    </span>
  )
}

function LiveBadge() {
  return (
    <span
      className="inline-flex items-center gap-1.5 rounded border border-signal-live/40 bg-signal-live/10 px-2 py-1 font-mono text-2xs font-semibold uppercase tracking-widest text-signal-live"
      title="Real-time telemetry is streaming"
    >
      <span className="live-dot" aria-hidden />
      Live
    </span>
  )
}
