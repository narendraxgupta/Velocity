/**
 * Submission orderbook replay viewer.
 *
 * Renders the L2 depth chart for the submission's reference orderbook at
 * a user-scrubbed timestamp. The validator pushes a snapshot to Redis
 * every 100ms (see services/correctness-validator/src/validator.cpp); we
 * proxy through the gateway, cache the recent samples client-side, and
 * let the user step / play / scrub through them.
 *
 * Why this is useful: when a submission misprices the spread or shows
 * stale depth, judges can pinpoint the exact moment the engine diverged
 * from the reference book without having to grep raw Kafka tape.
 */

'use client'

import Link from 'next/link'
import { useParams } from 'next/navigation'
import { useState } from 'react'

import { DepthChart } from '@/components/orderbook/depth-chart'
import { TimelineScrubber } from '@/components/orderbook/timeline-scrubber'
import {
  Panel,
  PanelBody,
  PanelDescription,
  PanelHeader,
  PanelTitle,
} from '@/components/ui/panel'
import { useOrderbookReplay } from '@/lib/hooks/use-orderbook-replay'
import { middleTruncate } from '@/lib/utils'

export default function OrderbookPage() {
  const params = useParams<{ id: string }>()
  const id = params?.id ?? ''
  const { timeline, current, loading, error, seek } = useOrderbookReplay(id)
  const [elapsedMs, setElapsedMs] = useState<number>(0)

  return (
    <div className="container space-y-6 py-8">
      <header className="flex flex-wrap items-end justify-between gap-3">
        <div>
          <span className="label-eyebrow">submission / orderbook replay</span>
          <h1 className="font-display text-2xl font-semibold tracking-tight">
            {middleTruncate(id, 28)}
          </h1>
          <p className="text-sm text-muted-foreground">
            Scrub through the reference orderbook snapshots captured by the
            correctness validator (10 Hz cadence).
          </p>
        </div>
        <Link
          href={`/submissions/${encodeURIComponent(id)}`}
          className="text-sm font-medium text-accent hover:underline"
        >
          ← back to submission
        </Link>
      </header>

      <Panel>
        <PanelHeader>
          <PanelTitle>Timeline</PanelTitle>
          <PanelDescription>
            {timeline.length === 0
              ? 'Waiting for the validator to publish the first snapshot…'
              : `${timeline.length} snapshot${timeline.length === 1 ? '' : 's'} available`}
          </PanelDescription>
        </PanelHeader>
        <PanelBody>
          <TimelineScrubber
            timeline={timeline}
            value={elapsedMs}
            onChange={(v) => {
              setElapsedMs(v)
              seek(v)
            }}
          />
        </PanelBody>
      </Panel>

      <Panel>
        <PanelHeader>
          <PanelTitle>L2 depth</PanelTitle>
          <PanelDescription>
            Cumulative bid (green) and ask (red) depth at the scrubbed timestamp.
            Prices are shown in display units (assumes scale 6 — adjust below
            for venues with a different price-scale).
          </PanelDescription>
        </PanelHeader>
        <PanelBody>
          {error && (
            <div className="mb-3 rounded border border-signal-ask/40 bg-signal-ask/5 p-3 font-mono text-2xs text-signal-ask">
              {error}
            </div>
          )}
          {current ? (
            <DepthChart bids={current.bids} asks={current.asks} />
          ) : (
            <div className="flex h-72 items-center justify-center font-mono text-2xs text-muted-foreground">
              {loading ? 'fetching snapshot…' : 'no snapshot loaded'}
            </div>
          )}
        </PanelBody>
      </Panel>
    </div>
  )
}
