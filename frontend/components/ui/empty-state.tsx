/**
 * EmptyState — used wherever a panel or page has no data to render.
 *
 * The illustration is intentionally minimal (no clip-art): we want the page
 * to feel like a Bloomberg terminal that *would* have data, not a marketing
 * landing page.
 */

import * as React from 'react'

import { cn } from '@/lib/utils'

type EmptyStateProps = {
  title: string
  description?: React.ReactNode
  icon?: React.ReactNode
  action?: React.ReactNode
  className?: string
}

export function EmptyState({ title, description, icon, action, className }: EmptyStateProps) {
  return (
    <div
      className={cn(
        'flex flex-col items-center justify-center gap-3 rounded-md border border-dashed border-border-subtle bg-surface/40 px-6 py-10 text-center',
        className,
      )}
    >
      {icon ? (
        <div
          className="flex h-10 w-10 items-center justify-center rounded-full border border-border-subtle bg-surface text-muted-foreground"
          aria-hidden
        >
          {icon}
        </div>
      ) : null}
      <h3 className="font-display text-base font-semibold tracking-tight">{title}</h3>
      {description ? (
        <p className="max-w-md text-sm text-muted-foreground">{description}</p>
      ) : null}
      {action ? <div className="pt-2">{action}</div> : null}
    </div>
  )
}
