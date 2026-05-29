/**
 * TimelineScrubber — range-slider + play/pause + step buttons for the
 * orderbook replay viewer.
 *
 * The slider's value is the elapsed_ms of the currently-displayed
 * snapshot. The min/max are derived from the timeline list; the slider
 * does NOT snap to known samples — the parent hook handles that.
 *
 * Play mode advances `value` by one timeline step every `intervalMs`
 * (default 100ms = real-time when snapshots are 10 Hz).
 */

'use client'

import { useEffect, useRef, useState } from 'react'

interface Props {
  timeline: number[]
  value: number
  onChange: (elapsedMs: number) => void
  intervalMs?: number
}

export function TimelineScrubber({
  timeline,
  value,
  onChange,
  intervalMs = 100,
}: Props) {
  const [playing, setPlaying] = useState(false)
  const playRef = useRef<ReturnType<typeof setInterval> | null>(null)

  useEffect(() => {
    if (!playing) {
      if (playRef.current) {
        clearInterval(playRef.current)
        playRef.current = null
      }
      return
    }
    if (timeline.length === 0) return
    playRef.current = setInterval(() => {
      const idx = timeline.findIndex((t) => t >= value)
      const next = timeline[Math.min(timeline.length - 1, idx === -1 ? 0 : idx + 1)]!
      if (next === value) {
        // At the end — pause.
        setPlaying(false)
        return
      }
      onChange(next)
    }, intervalMs)
    return () => {
      if (playRef.current) clearInterval(playRef.current)
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [playing, timeline.length, value])

  const min = timeline[0] ?? 0
  const max = timeline[timeline.length - 1] ?? 0
  const disabled = timeline.length === 0

  const step = (dir: 1 | -1) => {
    if (disabled) return
    const idx = Math.max(0, timeline.findIndex((t) => t >= value))
    const ni = Math.min(timeline.length - 1, Math.max(0, idx + dir))
    onChange(timeline[ni]!)
  }

  return (
    <div className="space-y-2">
      <div className="flex items-center gap-3">
        <button
          type="button"
          onClick={() => step(-1)}
          disabled={disabled}
          className="rounded border border-border px-2 py-0.5 font-mono text-2xs uppercase tracking-widest text-muted-foreground hover:text-foreground disabled:opacity-30"
        >
          ◀ step
        </button>
        <button
          type="button"
          onClick={() => setPlaying((p) => !p)}
          disabled={disabled}
          className="rounded border border-accent/40 px-2.5 py-0.5 font-mono text-2xs uppercase tracking-widest text-accent hover:bg-accent/10 disabled:opacity-30"
        >
          {playing ? '⏸ pause' : '▶ play'}
        </button>
        <button
          type="button"
          onClick={() => step(1)}
          disabled={disabled}
          className="rounded border border-border px-2 py-0.5 font-mono text-2xs uppercase tracking-widest text-muted-foreground hover:text-foreground disabled:opacity-30"
        >
          step ▶
        </button>
        <div className="ml-auto font-mono text-2xs text-muted-foreground">
          t = <span className="text-foreground">{formatMs(value)}</span>
          {' '}of{' '}
          <span className="text-foreground">{formatMs(max)}</span>
        </div>
      </div>

      <input
        type="range"
        min={min}
        max={max}
        step={1}
        disabled={disabled}
        value={value}
        onChange={(e) => onChange(Number(e.target.value))}
        className="w-full accent-accent"
      />
    </div>
  )
}

function formatMs(ms: number): string {
  if (ms < 1_000) return `${ms}ms`
  return `${(ms / 1000).toFixed(2)}s`
}
