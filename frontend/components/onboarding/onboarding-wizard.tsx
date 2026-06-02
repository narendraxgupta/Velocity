/**
 * <OnboardingWizard>
 *
 * Shown ONCE on first visit (gated by a localStorage flag), or on
 * demand via the help menu. Four small steps:
 *
 *   1. "What is Velocity" — one paragraph + a video link.
 *   2. "Set your team display name" — POST /v1/profile (no-op stub
 *      today, but the API is in place for Phase 5+).
 *   3. "Drop in a sample submission" — wires the user to the sample
 *      matching-engine template repo with a "Use this template" link.
 *   4. "Show me the leaderboard" — closes the wizard.
 *
 * We deliberately DON'T require completing all steps — the user can
 * dismiss at any point. The flag is set whether they finish or skip;
 * we want the wizard to feel optional, not mandatory.
 */

'use client'

import { usePathname } from 'next/navigation'
import { useEffect, useState } from 'react'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'

const STORAGE_KEY = 'velocity:onboarding:done:v1'

type Step = {
  id: string
  title: string
  body: React.ReactNode
  cta?: { label: string; href?: string; onClick?: () => void }
}

const STEPS: Step[] = [
  {
    id: 'welcome',
    title: 'Welcome to Velocity',
    body: (
      <div className="space-y-2 text-sm">
        <p>
          Velocity benchmarks low-latency trading systems against a
          realistic stochastic market, with chaos, plugin-extensible
          microstructure validators, and a public-leaderboard scoring
          model.
        </p>
        <p>
          This wizard takes ~90 seconds. You can dismiss it at any
          point and reopen it from the help menu.
        </p>
      </div>
    ),
  },
  {
    id: 'submit',
    title: 'Drop in a sample submission',
    body: (
      <div className="space-y-3 text-sm">
        <p>
          Want to see what a real submission looks like? Start from
          the reference matching engine template — it includes a
          Dockerfile, a basic FIFO matcher, and a smoke test.
        </p>
        <ol className="ml-4 list-decimal space-y-1.5 text-sm">
          <li>
            <a
              href="https://github.com/narendraxgupta/Velocity/tree/main/scripts/sample-exchange"
              target="_blank"
              rel="noopener noreferrer"
              className="text-accent hover:underline"
            >
              Open the template repo
            </a>{' '}
            and click <em>Use this template</em>.
          </li>
          <li>Edit <code className="font-mono">src/match.cpp</code>.</li>
          <li>
            From your fork:{' '}
            <code className="font-mono">velocity submit --team your-team --display your-engine</code>
          </li>
        </ol>
      </div>
    ),
    cta: {
      label: 'Open template repo →',
      href: 'https://github.com/narendraxgupta/Velocity/tree/main/scripts/sample-exchange',
    },
  },
  {
    id: 'profile',
    title: 'Pick your benchmark profile',
    body: (
      <div className="space-y-2 text-sm">
        <p>
          Six built-in profiles, from <em>baseline</em> (50k rps, balanced
          flow) to <em>cliff-finder</em> (5k → 1.28M rps staircase that
          pinpoints your breaking point with a confidence interval).
        </p>
        <p>
          Velocity also auto-suggests the next profile based on what
          went wrong in your last run; look for the{' '}
          <em>Adaptive next run</em> panel on the submission page.
        </p>
      </div>
    ),
  },
  {
    id: 'leaderboard',
    title: 'See where you stand',
    body: (
      <div className="space-y-2 text-sm">
        <p>
          The leaderboard updates in real time as benchmarks complete.
          Each row carries a composite score, a health badge (anomaly
          detector), and a latency-regression flag if your most recent
          run drifted from prior history.
        </p>
        <p>
          Click any row to drill into the run report — flamegraph,
          order-book replay, LLM critique, and more.
        </p>
      </div>
    ),
    cta: { label: 'Open the leaderboard →', href: '/leaderboard' },
  },
]

export function OnboardingWizard() {
  const [open, setOpen] = useState(false)
  const [stepIdx, setStepIdx] = useState(0)
  const pathname = usePathname()

  useEffect(() => {
    // Don't pop on public share pages — viewers without accounts
    // shouldn't see a wizard nudging them through a flow they have
    // no access to. We have to actively close (not just early-return)
    // because the layout keeps the wizard mounted across navigations,
    // so an already-open dialog would remain visible on top of the
    // share page.
    if (pathname?.startsWith('/share/')) {
      setOpen(false)
      return
    }
    try {
      const done = window.localStorage.getItem(STORAGE_KEY)
      if (!done) setOpen(true)
    } catch {
      // Sandbox / disabled storage — show the wizard once per page load.
      setOpen(true)
    }
  }, [pathname])

  const dismiss = () => {
    try { window.localStorage.setItem(STORAGE_KEY, 'true') } catch {}
    setOpen(false)
  }

  if (!open) return null

  const step = STEPS[stepIdx] ?? STEPS[0]!
  const isLast = stepIdx === STEPS.length - 1

  return (
    <div
      role="dialog"
      aria-modal="true"
      className="fixed inset-0 z-50 flex items-center justify-center bg-background/80 p-4 backdrop-blur-sm"
    >
      <div className="w-full max-w-xl">
        <Panel>
          <PanelHeader>
            <div className="flex items-center justify-between">
              <PanelTitle>{step.title}</PanelTitle>
              <button
                onClick={dismiss}
                className="rounded-md border border-border bg-surface px-2 py-0.5 font-mono text-2xs uppercase tracking-widest text-muted-foreground hover:text-foreground"
              >
                skip
              </button>
            </div>
            <PanelDescription>
              Step {stepIdx + 1} of {STEPS.length}
            </PanelDescription>
          </PanelHeader>
          <PanelBody>{step.body}</PanelBody>
          <div className="flex items-center justify-between border-t border-border-subtle px-5 py-3">
            <div className="flex gap-1">
              {STEPS.map((s, i) => (
                <span
                  key={s.id}
                  className={`h-1.5 w-6 rounded ${i === stepIdx ? 'bg-accent' : 'bg-border'}`}
                />
              ))}
            </div>
            <div className="flex items-center gap-2">
              {step.cta && (
                step.cta.href ? (
                  <a
                    href={step.cta.href}
                    target={step.cta.href.startsWith('http') ? '_blank' : undefined}
                    rel={step.cta.href.startsWith('http') ? 'noopener noreferrer' : undefined}
                    className="rounded-md border border-accent bg-accent/10 px-3 py-1.5 text-2xs font-semibold uppercase tracking-widest text-accent hover:bg-accent/20"
                  >
                    {step.cta.label}
                  </a>
                ) : (
                  <button
                    onClick={step.cta.onClick}
                    className="rounded-md border border-accent bg-accent/10 px-3 py-1.5 text-2xs font-semibold uppercase tracking-widest text-accent"
                  >
                    {step.cta.label}
                  </button>
                )
              )}
              {!isLast ? (
                <button
                  onClick={() => setStepIdx((i) => i + 1)}
                  className="rounded-md bg-accent px-3 py-1.5 text-2xs font-semibold uppercase tracking-widest text-background"
                >
                  Next
                </button>
              ) : (
                <button
                  onClick={dismiss}
                  className="rounded-md bg-accent px-3 py-1.5 text-2xs font-semibold uppercase tracking-widest text-background"
                >
                  Finish
                </button>
              )}
            </div>
          </div>
        </Panel>
      </div>
    </div>
  )
}
