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

import {
  areaGradient,
  AXIS_LABEL,
  AXIS_LINE_SUBTLE,
  CHART_COLORS,
  CROSSHAIR,
  glowLine,
  LEGEND_BASE,
  SPLIT_LINE_SUBTLE,
  TOOLTIP_BASE,
} from '@/lib/charts/theme'

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
    const lastTs = samples[samples.length - 1]?.tsMs ?? 0

    return {
      cliff: cliffPoint,
      option: {
        animation: false,
        grid: { left: 60, right: 14, top: 28, bottom: 34 },
        backgroundColor: 'transparent',
        tooltip: {
          trigger: 'axis',
          ...TOOLTIP_BASE,
          axisPointer: CROSSHAIR,
          valueFormatter: (v: number) => `${formatRps(v)} rps`,
        },
        legend: { ...LEGEND_BASE },
        xAxis: {
          type: 'time',
          axisLine: AXIS_LINE_SUBTLE,
          axisLabel: AXIS_LABEL,
          axisPointer: { label: { formatter: '' } },
          splitLine: { show: false },
        },
        yAxis: {
          type: 'value',
          axisLine: { show: false },
          axisLabel: { ...AXIS_LABEL, formatter: (v: number) => formatRps(v) },
          axisPointer: { label: { formatter: (p: { value: number }) => formatRps(p.value) } },
          splitLine: SPLIT_LINE_SUBTLE,
        },
        series: [
          {
            name: 'Offered',
            type: 'line',
            symbol: 'none',
            smooth: true,
            lineStyle: { width: 1.5, color: CHART_COLORS.violet, type: 'dashed', opacity: 0.85 },
            data: offered,
          },
          {
            name: 'Sustained',
            type: 'line',
            symbol: 'none',
            smooth: true,
            areaStyle: { color: areaGradient('rgba(16,240,149,0.28)', 'rgba(16,240,149,0)') },
            lineStyle: glowLine(CHART_COLORS.live, 2.5, 14),
            data: sustained,
            markLine: cliffPoint
              ? {
                  symbol: 'none',
                  label: {
                    color: CHART_COLORS.amber,
                    fontFamily: 'var(--font-mono, monospace)',
                    fontSize: 10,
                    formatter: 'CLIFF',
                  },
                  lineStyle: { color: CHART_COLORS.amber, type: 'dashed', width: 1.5 },
                  data: [{ xAxis: cliffPoint.tsMs }],
                }
              : undefined,
            // Shade the post-cliff region red — the zone where the engine
            // under test stopped keeping up with offered load.
            markArea: cliffPoint
              ? {
                  silent: true,
                  itemStyle: { color: 'rgba(244,63,94,0.07)' },
                  data: [[{ xAxis: cliffPoint.tsMs }, { xAxis: lastTs }]],
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
