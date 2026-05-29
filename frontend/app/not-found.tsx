/**
 * 404 — sober, dense, on-brand. No 404-illustration cliché.
 */

import Link from 'next/link'

import { EmptyState } from '@/components/ui/empty-state'

export default function NotFound() {
  return (
    <div className="container py-16">
      <EmptyState
        title="Route not found"
        description="We didn't recognise that URL. The platform's primary screens are linked from the top nav."
        action={
          <Link
            href="/"
            className="inline-flex items-center gap-1 rounded border border-border bg-surface px-3 py-1.5 text-xs font-medium hover:bg-surface-elevated"
          >
            Go back to overview
          </Link>
        }
      />
    </div>
  )
}
