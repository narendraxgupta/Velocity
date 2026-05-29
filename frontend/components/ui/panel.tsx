/**
 * Panel — the universal container primitive.
 *
 * Three slots, all optional:
 *   <Panel>
 *     <PanelHeader>
 *       <PanelTitle>…</PanelTitle>
 *       <PanelDescription>…</PanelDescription>
 *     </PanelHeader>
 *     <PanelBody>…</PanelBody>
 *     <PanelFooter>…</PanelFooter>
 *   </Panel>
 *
 * Lifted from the shadcn/ui pattern but simplified for our domain — Velocity
 * does not have cards in the e-commerce sense; it has dashboard panels.
 */

import * as React from 'react'

import { cn } from '@/lib/utils'

const Panel = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => (
    <div
      ref={ref}
      className={cn(
        'flex flex-col overflow-hidden rounded-md border border-border bg-surface text-foreground shadow-panel-sm',
        className,
      )}
      {...props}
    />
  ),
)
Panel.displayName = 'Panel'

const PanelHeader = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => (
    <div ref={ref} className={cn('flex flex-col gap-1 px-4 py-3', className)} {...props} />
  ),
)
PanelHeader.displayName = 'PanelHeader'

const PanelTitle = React.forwardRef<HTMLHeadingElement, React.HTMLAttributes<HTMLHeadingElement>>(
  ({ className, ...props }, ref) => (
    <h3
      ref={ref}
      className={cn('font-display text-base font-semibold leading-tight tracking-tight', className)}
      {...props}
    />
  ),
)
PanelTitle.displayName = 'PanelTitle'

const PanelDescription = React.forwardRef<
  HTMLParagraphElement,
  React.HTMLAttributes<HTMLParagraphElement>
>(({ className, ...props }, ref) => (
  <p ref={ref} className={cn('text-sm leading-snug text-muted-foreground', className)} {...props} />
))
PanelDescription.displayName = 'PanelDescription'

const PanelBody = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => (
    <div ref={ref} className={cn('px-4 py-3', className)} {...props} />
  ),
)
PanelBody.displayName = 'PanelBody'

const PanelFooter = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => (
    <div
      ref={ref}
      className={cn('flex items-center justify-between border-t border-border-subtle px-4 py-3', className)}
      {...props}
    />
  ),
)
PanelFooter.displayName = 'PanelFooter'

export { Panel, PanelHeader, PanelTitle, PanelDescription, PanelBody, PanelFooter }
