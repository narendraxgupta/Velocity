/**
 * Skeleton — neutral placeholder block used while data is loading. Animates
 * via a CSS keyframe; SSR friendly. Compose with `className` to size.
 */

import { cn } from '@/lib/utils'

export function Skeleton({ className, ...props }: React.HTMLAttributes<HTMLDivElement>) {
  return (
    <div
      className={cn(
        'rounded-md bg-surface-elevated/60 animate-pulse',
        className,
      )}
      {...props}
    />
  )
}

export function SkeletonText({ lines = 3, className }: { lines?: number; className?: string }) {
  return (
    <div className={cn('space-y-2', className)}>
      {Array.from({ length: lines }, (_, i) => (
        <Skeleton
          key={i}
          className={cn('h-3', i === lines - 1 ? 'w-2/3' : 'w-full')}
        />
      ))}
    </div>
  )
}

export function SkeletonRow({ columns = 6 }: { columns?: number }) {
  return (
    <div className="flex items-center gap-4">
      {Array.from({ length: columns }, (_, i) => (
        <Skeleton key={i} className={cn('h-4 flex-1', i === 0 ? 'max-w-[3rem]' : '')} />
      ))}
    </div>
  )
}
