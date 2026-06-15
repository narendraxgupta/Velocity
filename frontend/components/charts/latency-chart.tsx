/**
 * LatencyChart — small ECharts panel showing p50/p90/p99/p999 over time.
 *
 * Designed for the submission detail page. Accepts the BenchmarkSnapshot
 * SSE stream's payload directly and pushes the latest 600 points (10 min
 * at one sample per second) onto a rolling window.
 *
 * We keep the chart deliberately spartan — log-scale y-axis (because
 * latency spans 4-5 decades), high-contrast lines, and no animation
 * during updates (jitters the eye on every tick).
 */

'use client'

import dynamic from 'next/dynamic'
import { useMemo, useRef } from 'react'

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

// echarts-for-react is client-only; load lazily.
const ReactECharts = dynamic(() => import('echarts-for-react'), { ssr: false })

export type LatencySample = {
  tsMs: number
  p50Ns: number
  p90Ns: number
  p99Ns: number
  p999Ns: number
  /** Kernel-side p50/p99 from the eBPF probe. Both 0 → series hidden. */
  kernelP50Ns?: number
  kernelP99Ns?: number
  kernelSamples?: number
}

export function LatencyChart({ samples }: { samples: LatencySample[] }) {
  const lastTs = useRef<number>(0)

  // Are any of the samples carrying kernel-side data? We only draw the
  // dashed overlay when at least one sample observed kernel events;
  // otherwise the chart stays clean.
  const hasKernel = samples.some((s) => (s.kernelSamples ?? 0) > 0)

  const option = useMemo(() => {
    const lineData = (k: keyof Omit<LatencySample, 'tsMs'>) =>
      samples.map((s) => [s.tsMs, ((s[k] as number | undefined) ?? 0) / 1_000_000])  // ms

    return {
      animation: false,
      grid: { left: 52, right: 14, top: 28, bottom: 30 },
      backgroundColor: 'transparent',
      tooltip: {
        trigger: 'axis',
        ...TOOLTIP_BASE,
        axisPointer: CROSSHAIR,
        valueFormatter: (v: number) => formatMs(v),
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
        type: 'log',
        logBase: 10,
        axisLine: { show: false },
        axisLabel: { ...AXIS_LABEL, formatter: (v: number) => formatMs(v) },
        axisPointer: { label: { formatter: (p: { value: number }) => formatMs(p.value) } },
        splitLine: SPLIT_LINE_SUBTLE,
      },
      series: [
        { name: 'p50',  type: 'line', symbol: 'none', smooth: true,
          lineStyle: glowLine(CHART_COLORS.cyan, 1.5, 6), data: lineData('p50Ns') },
        { name: 'p90',  type: 'line', symbol: 'none', smooth: true,
          lineStyle: glowLine(CHART_COLORS.violet, 1.5, 6), data: lineData('p90Ns') },
        // p99 is the headline series — brighter glow + a gradient fill so the
        // tail "mountain" reads at a glance.
        { name: 'p99',  type: 'line', symbol: 'none', smooth: true,
          lineStyle: glowLine(CHART_COLORS.amber, 2.5, 14),
          areaStyle: { color: areaGradient('rgba(255,169,64,0.22)', 'rgba(255,169,64,0)'), origin: 'start' },
          data: lineData('p99Ns') },
        { name: 'p999', type: 'line', symbol: 'none', smooth: true,
          lineStyle: glowLine(CHART_COLORS.ask, 2, 10), data: lineData('p999Ns') },
        // Kernel-side overlay (eBPF). Dashed lines + muted colors so they
        // visually nest underneath the userspace series. The delta between
        // matching pairs (userspace_p99 − kernel_p99) is the time the
        // load-gen + transport itself contributed — i.e. the bot worker's
        // own cost rather than the engine under test.
        ...(hasKernel
          ? [
              {
                name: 'p50 (kernel)',
                type: 'line',
                symbol: 'none',
                smooth: true,
                lineStyle: { width: 1.5, color: CHART_COLORS.cyan, type: 'dashed', opacity: 0.55 },
                data: lineData('kernelP50Ns'),
              },
              {
                name: 'p99 (kernel)',
                type: 'line',
                symbol: 'none',
                smooth: true,
                lineStyle: { width: 2, color: CHART_COLORS.amber, type: 'dashed', opacity: 0.55 },
                data: lineData('kernelP99Ns'),
              },
            ]
          : []),
      ],
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [samples, hasKernel])

  // ECharts re-renders on every option change; suppress its built-in
  // animation so we get a clean rolling-window effect.
  const tail = samples[samples.length - 1]
  if (tail) lastTs.current = tail.tsMs

  return (
    <div className="h-64 w-full">
      <ReactECharts
        option={option}
        notMerge
        lazyUpdate
        style={{ height: '100%', width: '100%' }}
      />
    </div>
  )
}

function formatMs(v: number): string {
  if (v < 0.001) return `${(v * 1_000_000).toFixed(0)}ns`
  if (v < 1) return `${(v * 1000).toFixed(0)}µs`
  if (v < 1000) return `${v.toFixed(v < 10 ? 2 : 1)}ms`
  return `${(v / 1000).toFixed(2)}s`
}
