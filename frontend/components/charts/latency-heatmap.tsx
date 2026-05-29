/**
 * LatencyHeatmap — time × latency density visualization.
 *
 * This is the canonical chart for microsecond-honest tail-latency analysis
 * (Honeycomb / Lightstep / Datadog all ship the same thing). For each 1-second
 * column on the x-axis we render a vertical density slice across log-spaced
 * latency buckets on the y-axis, coloured by the *probability mass* that fell
 * into each bucket.
 *
 * The benchmark SSE stream gives us five percentile crossings per snapshot
 * (p50, p90, p99, p999, max). We reconstruct an approximate density from those
 * crossings using the well-known percentile-mass weighting:
 *
 *    bucket containing p50  receives 0.50 mass
 *    bucket containing p90  receives 0.40 mass
 *    bucket containing p99  receives 0.09 mass
 *    bucket containing p999 receives 0.009 mass
 *    bucket containing max  receives 0.001 mass
 *
 * Once HdrHistogram bucket vectors are surfaced end-to-end (proto change
 * pending in scoring.proto -> BenchmarkSnapshot.hdr_buckets), this component
 * will switch to reading them directly — the rendering pipeline stays
 * identical, only the bucket source changes.
 *
 * Why log spacing? Real latency distributions in trading systems span 5-6
 * decades (5µs at the floor, multi-second tails when GC pauses fire). A
 * linear axis collapses everything interesting into the bottom 1% of pixels.
 */

'use client'

import dynamic from 'next/dynamic'
import { useMemo } from 'react'

const ReactECharts = dynamic(() => import('echarts-for-react'), { ssr: false })

export type LatencyHeatmapSample = {
  tsMs: number
  p50Ns: number
  p90Ns: number
  p99Ns: number
  p999Ns: number
  maxNs: number
}

// 4 buckets per decade × 7 decades (1µs → 10s) = 28 buckets.
const BUCKET_COUNT = 28
const NS_FLOOR = 1_000           // 1 µs
const NS_CEIL  = 10_000_000_000  // 10 s
const LN_FLOOR = Math.log(NS_FLOOR)
const LN_CEIL  = Math.log(NS_CEIL)
const BUCKETS = Array.from({ length: BUCKET_COUNT }, (_, i) => {
  // Bucket midpoint in ns (geometric mean of its lower/upper bound).
  const lo = Math.exp(LN_FLOOR + ((LN_CEIL - LN_FLOOR) * i) / BUCKET_COUNT)
  const hi = Math.exp(LN_FLOOR + ((LN_CEIL - LN_FLOOR) * (i + 1)) / BUCKET_COUNT)
  return { lo, hi, mid: Math.sqrt(lo * hi) }
})

const PERCENTILE_MASS: Array<{ key: keyof Omit<LatencyHeatmapSample, 'tsMs'>; weight: number }> = [
  { key: 'p50Ns',  weight: 0.500 },
  { key: 'p90Ns',  weight: 0.400 },
  { key: 'p99Ns',  weight: 0.090 },
  { key: 'p999Ns', weight: 0.009 },
  { key: 'maxNs',  weight: 0.001 },
]

function bucketIndex(ns: number): number {
  if (ns <= NS_FLOOR) return 0
  if (ns >= NS_CEIL)  return BUCKET_COUNT - 1
  const idx = Math.floor(((Math.log(ns) - LN_FLOOR) / (LN_CEIL - LN_FLOOR)) * BUCKET_COUNT)
  return Math.max(0, Math.min(BUCKET_COUNT - 1, idx))
}

function formatNs(ns: number): string {
  if (ns < 1_000)             return `${ns.toFixed(0)}ns`
  if (ns < 1_000_000)         return `${(ns / 1_000).toFixed(0)}µs`
  if (ns < 1_000_000_000)     return `${(ns / 1_000_000).toFixed(1)}ms`
  return `${(ns / 1_000_000_000).toFixed(1)}s`
}

export function LatencyHeatmap({ samples }: { samples: LatencyHeatmapSample[] }) {
  const option = useMemo(() => {
    if (samples.length === 0) {
      return { backgroundColor: 'transparent' }
    }

    // ECharts heatmap data: [xIndex, yIndex, value]. We use indices because
    // mixing 'time' on x with 'category' on y triggers tooltip glitches.
    //
    // tsconfig has `noUncheckedIndexedAccess: true`, so every `arr[i]`
    // lookup widens to `T | undefined`. Loops below are bounded by
    // `.length` / `BUCKET_COUNT`, so the indices are always in range —
    // we narrow via local `const` bindings instead of sprinkling `!`
    // non-null assertions through the body.
    const data: Array<[number, number, number]> = []
    let peakMass = 0
    for (let xi = 0; xi < samples.length; xi++) {
      const s = samples[xi]
      if (!s) continue
      const slice = new Array<number>(BUCKET_COUNT).fill(0)
      for (const { key, weight } of PERCENTILE_MASS) {
        const v = s[key]
        if (v && v > 0) {
          const bi = bucketIndex(v)
          slice[bi] = (slice[bi] ?? 0) + weight
        }
      }
      for (let yi = 0; yi < BUCKET_COUNT; yi++) {
        const mass = slice[yi] ?? 0
        if (mass > 0) {
          data.push([xi, yi, mass])
          if (mass > peakMass) peakMass = mass
        }
      }
    }

    return {
      animation: false,
      grid: { left: 56, right: 24, top: 16, bottom: 56 },
      backgroundColor: 'transparent',
      tooltip: {
        position: 'top',
        backgroundColor: 'rgba(15,16,20,0.94)',
        borderColor: 'rgba(255,255,255,0.06)',
        borderWidth: 1,
        textStyle: { color: '#e6e7ea', fontFamily: 'var(--font-mono, monospace)', fontSize: 11 },
        formatter: (p: { data: [number, number, number] }) => {
          const [xi, yi, mass] = p.data
          const ts = samples[xi]?.tsMs ?? 0
          const b = BUCKETS[yi]
          const tsLabel = new Date(ts).toLocaleTimeString([], { hour12: false })
          // `b` is `Bucket | undefined` under noUncheckedIndexedAccess.
          // In practice ECharts only ever calls the formatter with the
          // (xi, yi) tuples we shipped in `data`, where `yi` came from
          // a bounded loop — but a hover after the chart resized once
          // with a smaller bucket set CAN trip this. Degrade gracefully.
          if (!b) {
            return `${tsLabel}<br/>mass: ${(mass * 100).toFixed(1)}%`
          }
          return `${tsLabel}<br/>bucket: ${formatNs(b.lo)} → ${formatNs(b.hi)}<br/>mass: ${(mass * 100).toFixed(1)}%`
        },
      },
      xAxis: {
        type: 'category',
        data: samples.map((s) => new Date(s.tsMs).toLocaleTimeString([], { hour12: false })),
        axisLine: { lineStyle: { color: 'rgba(255,255,255,0.06)' } },
        axisLabel: {
          color: '#6b7077',
          fontFamily: 'var(--font-mono, monospace)',
          fontSize: 10,
          // Show ~8 evenly-spaced labels regardless of sample count.
          interval: Math.max(0, Math.floor(samples.length / 8) - 1),
        },
        splitLine: { show: false },
      },
      yAxis: {
        type: 'category',
        data: BUCKETS.map((b) => formatNs(b.mid)),
        inverse: false,
        axisLine: { show: false },
        axisLabel: {
          color: '#6b7077',
          fontFamily: 'var(--font-mono, monospace)',
          fontSize: 10,
          // Bucket labels would overlap; show every 4th (one per decade).
          interval: 3,
        },
        splitLine: { show: false },
      },
      visualMap: {
        type: 'continuous',
        min: 0,
        max: Math.max(0.001, peakMass),
        calculable: false,
        orient: 'horizontal',
        bottom: 0,
        right: 12,
        itemWidth: 12,
        itemHeight: 110,
        textStyle: { color: '#6b7077', fontFamily: 'var(--font-mono, monospace)', fontSize: 9 },
        // A perceptually-uniform dark-to-accent gradient. Avoids the jet
        // colourmap (banding + colour-blindness hazard).
        inRange: {
          color: [
            'rgba(20, 22, 28, 0.05)',
            'rgba(34, 211, 238, 0.35)',
            'rgba(124, 102, 255, 0.65)',
            'rgba(244, 63, 94, 0.95)',
          ],
        },
        text: ['hot', 'cold'],
      },
      series: [
        {
          type: 'heatmap',
          data,
          itemStyle: { borderRadius: 1 },
          progressive: 5000,
          progressiveThreshold: 8000,
          emphasis: {
            itemStyle: { borderColor: '#7c66ff', borderWidth: 1 },
          },
        },
      ],
    }
  }, [samples])

  return (
    <div className="h-72 w-full">
      <ReactECharts
        option={option}
        notMerge
        lazyUpdate
        style={{ height: '100%', width: '100%' }}
      />
    </div>
  )
}
