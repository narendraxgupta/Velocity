/**
 * Global error boundary for the app. Next.js mounts this when an unhandled
 * exception bubbles through any server component or root client component.
 */

'use client'

import Link from 'next/link'
import { useEffect } from 'react'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'

export default function GlobalError({
  error,
  reset,
}: {
  error: Error & { digest?: string }
  reset: () => void
}) {
  useEffect(() => {
    // Surface to the console so it's captured in dev-tools and any
    // browser-side log shipper that watches stderr.
    // eslint-disable-next-line no-console
    console.error('[velocity] route error', error)
  }, [error])

  return (
    <div className="container py-12">
      <Panel>
        <PanelHeader>
          <PanelTitle className="text-signal-warn">Something broke on this route</PanelTitle>
          <PanelDescription>
            The rest of the app is still running. Try the action below, or jump back to the
            leaderboard.
          </PanelDescription>
        </PanelHeader>
        <PanelBody className="space-y-4">
          <pre className="max-h-64 overflow-auto rounded border border-border-subtle bg-background/80 p-3 font-mono text-2xs text-foreground/80">
{String(error.message || error)}
{error.digest ? `\n\ndigest: ${error.digest}` : ''}
          </pre>
          <div className="flex items-center gap-2">
            <button
              type="button"
              onClick={reset}
              className="rounded border border-border bg-surface px-3 py-1.5 text-xs font-medium hover:bg-surface-elevated"
            >
              Reset
            </button>
            <Link
              href="/leaderboard"
              className="rounded border border-border-subtle bg-surface px-3 py-1.5 text-xs font-medium text-muted-foreground hover:bg-surface-elevated hover:text-foreground"
            >
              Go to leaderboard
            </Link>
          </div>
        </PanelBody>
      </Panel>
    </div>
  )
}
