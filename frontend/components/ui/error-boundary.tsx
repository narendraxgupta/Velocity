/**
 * ErrorBoundary — last-resort UI safety net.
 *
 * Next.js 14 already supplies `error.tsx` per-route, but we wrap data-driven
 * subtrees (live charts, websocket consumers) so a runtime explosion in one
 * widget doesn't take down the whole submission detail page.
 */

'use client'

import * as React from 'react'

import { Panel, PanelBody, PanelDescription, PanelHeader, PanelTitle } from '@/components/ui/panel'

type State = { error: Error | null }

type Props = {
  children: React.ReactNode
  title?: string
  reset?: () => void
  fallback?: (error: Error, reset: () => void) => React.ReactNode
}

export class ErrorBoundary extends React.Component<Props, State> {
  override state: State = { error: null }

  static getDerivedStateFromError(error: Error): State {
    return { error }
  }

  override componentDidCatch(error: Error, info: React.ErrorInfo) {
    // We deliberately avoid `console.error` (Next dev overlay would swallow
    // it) and emit a structured warn instead so subtree failures stay
    // surfaced without crashing the entire route.
    // eslint-disable-next-line no-console
    console.warn('[velocity] caught error in subtree', { error, componentStack: info.componentStack })
  }

  reset = () => {
    this.setState({ error: null })
    this.props.reset?.()
  }

  override render() {
    if (this.state.error) {
      if (this.props.fallback) return this.props.fallback(this.state.error, this.reset)
      return (
        <Panel>
          <PanelHeader>
            <PanelTitle className="text-signal-warn">
              {this.props.title ?? 'Something went sideways here'}
            </PanelTitle>
            <PanelDescription>
              The rest of the page is still usable. We&apos;ve logged this in the console.
            </PanelDescription>
          </PanelHeader>
          <PanelBody className="space-y-3">
            <pre className="overflow-x-auto rounded border border-border-subtle bg-background/80 p-3 font-mono text-2xs text-foreground/80">
{String(this.state.error.message || this.state.error)}
            </pre>
            <button
              type="button"
              onClick={this.reset}
              className="inline-flex items-center gap-1 rounded border border-border bg-surface px-3 py-1.5 text-xs font-medium hover:bg-surface-elevated"
            >
              Retry
            </button>
          </PanelBody>
        </Panel>
      )
    }
    return this.props.children
  }
}
