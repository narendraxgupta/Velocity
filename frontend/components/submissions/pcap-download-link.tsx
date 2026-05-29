/**
 * PcapDownloadLink — surfaces the recorded .pcap for a finished benchmark.
 *
 * The gateway exposes `GET /v1/benchmarks/{benchmark_id}/pcap`, which:
 *   - 200 → `{ download_url, size_bytes, expires_in }`
 *   - 404 → recorder never finalised this benchmark (still running,
 *           chaos-killed, pcap-replay submission, etc.)
 *
 * We lazy-fetch on hover instead of on mount so we don't hammer MinIO for
 * every leaderboard row. The link itself opens the presigned MinIO URL in
 * a new tab — browsers will trigger "save as" because of the
 * Content-Disposition the recorder sets on the object.
 */

'use client'

import { useCallback, useState } from 'react'
import { toast } from 'sonner'

import { apiFetch } from '@/lib/api/client'

type State =
  | { kind: 'idle' }
  | { kind: 'loading' }
  | { kind: 'ready'; url: string; sizeBytes: number }
  | { kind: 'missing' }
  | { kind: 'error'; message: string }

interface Props {
  /** Benchmark id (e.g. "BM-..."). Pass `null` while it's still unknown. */
  benchmarkId: string | null
}

export function PcapDownloadLink({ benchmarkId }: Props) {
  const [state, setState] = useState<State>({ kind: 'idle' })

  const fetchUrl = useCallback(async () => {
    if (!benchmarkId) return
    setState({ kind: 'loading' })
    try {
      const resp = await apiFetch(
        `/v1/benchmarks/${encodeURIComponent(benchmarkId)}/pcap`,
      )
      if (resp.status === 404) {
        setState({ kind: 'missing' })
        return
      }
      if (!resp.ok) {
        const detail = await resp.text()
        setState({ kind: 'error', message: detail || `HTTP ${resp.status}` })
        return
      }
      const body = (await resp.json()) as {
        download_url: string
        size_bytes: number
      }
      setState({ kind: 'ready', url: body.download_url, sizeBytes: body.size_bytes })
      // Auto-open: the user clicked Download → they want the file now.
      window.open(body.download_url, '_blank', 'noopener,noreferrer')
    } catch (err) {
      setState({ kind: 'error', message: (err as Error).message })
      toast.error('Pcap fetch failed', { description: (err as Error).message })
    }
  }, [benchmarkId])

  if (!benchmarkId) {
    return (
      <span
        className="text-sm font-medium text-muted-foreground/60"
        title="A benchmark must be in progress before a pcap is available"
      >
        Download .pcap
      </span>
    )
  }

  if (state.kind === 'loading') {
    return (
      <span className="text-sm font-medium text-muted-foreground">Fetching…</span>
    )
  }

  if (state.kind === 'missing') {
    return (
      <span
        className="text-sm font-medium text-muted-foreground/60"
        title="The recorder has not finalised a pcap for this benchmark yet"
      >
        .pcap pending
      </span>
    )
  }

  if (state.kind === 'ready') {
    return (
      <a
        href={state.url}
        target="_blank"
        rel="noopener noreferrer"
        className="text-sm font-medium text-accent hover:underline"
        title={`Download recorded TCP stream (${formatBytes(state.sizeBytes)})`}
      >
        Download .pcap ({formatBytes(state.sizeBytes)}) →
      </a>
    )
  }

  if (state.kind === 'error') {
    return (
      <button
        type="button"
        onClick={fetchUrl}
        className="text-sm font-medium text-signal-ask hover:underline"
        title={state.message}
      >
        Retry .pcap →
      </button>
    )
  }

  return (
    <button
      type="button"
      onClick={fetchUrl}
      className="text-sm font-medium text-accent hover:underline"
      title="Download the recorded TCP byte stream from this benchmark"
    >
      Download .pcap →
    </button>
  )
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MiB`
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GiB`
}
