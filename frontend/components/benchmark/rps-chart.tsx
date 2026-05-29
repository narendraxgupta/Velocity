/**
 * RpsChart — small ECharts panel showing offered vs sustained RPS.
 *
 * Auto-detects the throughput cliff: when current_rps stops scaling with
 * the ramp's intended target_rps, we mark that point with a vertical
 * dashed line. This is the answer to the problem statement's
 * "max TPS handled before failure" requirement.
 */

'use client'

import dynamic from 'next/dynamic'
import { useMemo } from 'react'

const ReactECharts = dynamic(() => import('echarts-for-react'), { ssr: false })

export type RpsSample = {
  tsMs: number
  currentRps: number
  targetRps: number
}

export function RpsChart({ samples }: { samples: RpsSample[] }) {
  const { option, cliff } = useMemo(() => {
    const offered = samples.map((s) => [s.tsMs, s.targetRps] as const)
    const sustained = samples.map((s) => [s.tsMs, s.currentRps] as const)
    const cliffPoint = detectCliff(samples)

    return {
      cliff: cliffPoint,
      option: {
        animation: false,
        grid: { left: 56, right: 12, top: 24, bottom: 32 },
        backgroundColor: 'transparent',
        tooltip: {
          trigger: 'axis',
          backgroundColor: 'rgba(15,16,20,0.94)',
          borderColor: 'rgba(255,255,255,0.06)',
          textStyle: { color: '#e6e7ea', fontFamily: 'var(--font-mono, monospace)', fontSize: 12 },
        },
        legend: {
          bottom: 0,
          textStyle: { color: '#9ba0a6', fontFamily: 'var(--font-mono, monospace)', fontSize: 10 },
          itemWidth: 12,
          itemHeight: 2,
        },
        xAxis: {
          type: 'time',
          axisLine: { lineStyle: { color: 'rgba(255,255,255,0.06)' } },
          axisLabel: { color: '#6b7077', fontFamily: 'var(--font-mono, monospace)', fontSize: 10 },
          splitLine: { show: false },
        },
        yAxis: {
          type: 'value',
          axisLine: { show: false },
          axisLabel: {
            color: '#6b7077',
            fontFamily: 'var(--font-mono, monospace)',
            fontSize: 10,
            formatter: (v: number) => formatRps(v),
          },
          splitLine: { lineStyle: { color: 'rgba(255,255,255,0.04)' } },
        },
        series: [
          {
            name: 'Offered',
            type: 'line',
            symbol: 'none',
            smooth: false,
            lineStyle: { width: 1.5, color: 'rgba(123, 154, 255, 0.7)' },
            data: offered,
          },
          {
            name: 'Sustained',
            type: 'line',
            symbol: 'none',
            smooth: false,
            areaStyle: { color: 'rgba(96, 250, 175, 0.10)' },
            lineStyle: { width: 2, color: '#60FAAF' },
            data: sustained,
            markLine: cliffPoint
              ? {
                  symbol: 'none',
                  label: {
                    color: '#fbbf24',
                    fontFamily: 'var(--font-mono, monospace)',
                    fontSize: 10,
                    formatter: 'cliff',
                  },
                  lineStyle: { color: '#fbbf24', type: 'dashed' },
                  data: [{ xAxis: cliffPoint.tsMs }],
                }
              : undefined,
          },
        ],
      },
    }
  }, [samples])

  return (
    <div className="space-y-2">
      <div className="h-56 w-full">
        <ReactECharts option={option} notMerge lazyUpdate style={{ height: '100%', width: '100%' }} />
      </div>
      {cliff && (
        <p className="px-1 font-mono text-2xs uppercase tracking-wider text-signal-warn">
          throughput cliff @ {formatRps(cliff.currentRps)} rps · {Math.round((cliff.currentRps / cliff.targetRps) * 100)}% of offered
        </p>
      )}
    </div>
  )
}

/* -------------------------------------------------------------------------- */

function detectCliff(samples: RpsSample[]): RpsSample | null {
  // Take any point during the HOLD phase where sustained drops below 80%
  // of offered, sustained for at least 2 samples.
  //
  // With noUncheckedIndexedAccess, indexed reads return `T | undefined`
  // even when bounded by .length. We bind once and bail on any miss —
  // a missing element in a triple-sample window means the loop indices
  // are off, and we'd rather skip the iteration than NaN-compare.
  for (let i = 2; i < samples.length; ++i) {
    const a = samples[i - 2]
    const b = samples[i - 1]
    const c = samples[i]
    if (!a || !b || !c) continue
    if (
      c.targetRps > 0 &&
      c.currentRps < c.targetRps * 0.8 &&
      b.currentRps < b.targetRps * 0.8 &&
      a.currentRps < a.targetRps * 0.8
    ) {
      return c
    }
  }
  return null
}

function formatRps(v: number): string {
  if (!Number.isFinite(v)) return '—'
  if (Math.abs(v) >= 1_000_000) return `${(v / 1_000_000).toFixed(1)}M`
  if (Math.abs(v) >= 1_000) return `${(v / 1_000).toFixed(1)}k`
  return v.toFixed(0)
}
