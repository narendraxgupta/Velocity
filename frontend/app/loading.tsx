/**
 * Top-level route loading state. Renders a skeleton chrome that matches the
 * usual page layout (header strip + two panels) so the page doesn't jump
 * when content arrives.
 */

import { Skeleton, SkeletonText } from '@/components/ui/skeleton'

export default function Loading() {
  return (
    <div className="container space-y-6 py-8">
      <div className="space-y-2">
        <Skeleton className="h-3 w-24" />
        <Skeleton className="h-6 w-72" />
        <Skeleton className="h-4 w-96" />
      </div>
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
        {Array.from({ length: 3 }, (_, i) => (
          <div key={i} className="rounded-md border border-border bg-surface p-4">
            <SkeletonText lines={5} />
          </div>
        ))}
      </div>
      <Skeleton className="h-64 w-full rounded-md" />
    </div>
  )
}
