/**
 * Hero — the landing-page banner. Restrained: a wordmark, the elevator pitch,
 * two calls to action. No video, no parallax, no gradient confetti.
 */

import Link from 'next/link'

import { ArrowRight, BookOpen } from 'lucide-react'

export function Hero() {
  return (
    <section
      aria-labelledby="hero-title"
      className="relative isolate overflow-hidden rounded-xl border border-border bg-gradient-hero"
    >
      {/* Fine grid behind the content — pure CSS, no SVG cost */}
      <div className="absolute inset-0 bg-grid opacity-40" aria-hidden />
      <div className="relative px-6 py-12 sm:px-10 sm:py-16">
        <span className="label-eyebrow">
          Distributed Benchmarking · Microsecond-honest tail latency
        </span>
        <h1
          id="hero-title"
          className="mt-3 max-w-3xl font-display text-3xl font-semibold tracking-tighter sm:text-4xl md:text-5xl"
        >
          Honest tail latency for trading infrastructure that has none to spare.
        </h1>
        <p className="mt-4 max-w-2xl text-md text-muted-foreground sm:text-lg">
          Submit a matching engine. We sandbox it, point a fleet of{' '}
          <code className="rounded bg-surface px-1 py-0.5 font-mono text-sm">io_uring</code> bots
          at it, measure with{' '}
          <code className="rounded bg-surface px-1 py-0.5 font-mono text-sm">HdrHistogram</code>,
          replay every fill against a reference orderbook, and publish the score live.
        </p>

        <div className="mt-7 flex flex-wrap items-center gap-3">
          <Link
            href="/submissions"
            className="inline-flex items-center gap-2 rounded-md bg-accent px-4 py-2 text-sm font-semibold text-accent-foreground shadow-glow-accent transition-transform hover:-translate-y-0.5"
          >
            Submit an engine
            <ArrowRight className="h-4 w-4" />
          </Link>
          <Link
            href="https://github.com/velocity/platform/blob/main/docs/architecture.md"
            className="inline-flex items-center gap-2 rounded-md border border-border bg-surface px-4 py-2 text-sm font-semibold text-foreground transition-colors hover:bg-surface-elevated"
          >
            <BookOpen className="h-4 w-4" />
            Read the architecture
          </Link>
        </div>

        {/* Inline mini-stats row — Bloomberg style */}
        <dl className="mt-10 grid grid-cols-2 gap-x-8 gap-y-4 border-t border-border-subtle pt-6 sm:grid-cols-4">
          <Stat label="Bot fleet" value="C++" sub="io_uring + uWS" />
          <Stat label="Sandbox" value="gVisor" sub="cgroups v2 · CPU-pinned" />
          <Stat label="Timestore" value="QuestDB" sub="ILP UDP · 4M rows/s" />
          <Stat label="Latency" value="HDR" sub="p50 · p90 · p99 · p99.9" />
        </dl>
      </div>
    </section>
  )
}

function Stat({ label, value, sub }: { label: string; value: string; sub: string }) {
  return (
    <div>
      <dt className="label-eyebrow">{label}</dt>
      <dd className="mt-1 font-display text-xl font-semibold tracking-tight">{value}</dd>
      <dd className="font-mono text-2xs uppercase tracking-wider text-muted-foreground">
        {sub}
      </dd>
    </div>
  )
}
