/**
 * DepthChart — L2 cumulative-depth chart with bids on the left and asks
 * on the right of an x-axis anchored on the mid price.
 *
 * Drawn with ECharts (already a project dependency from the latency
 * chart). The series are step-shaped (`step: 'end'`) because the depth
 * function is, by definition, a left-continuous staircase: each price
 * level adds its quantity to the cumulative total.
 */

'use client'

import dynamic from 'next/dynamic'
import { useMemo } from 'react'

import type { DepthRow } from '@/lib/hooks/use-orderbook-replay'
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

interface Props {
  bids: DepthRow[]      // sorted descending by price
  asks: DepthRow[]      // sorted ascending by price
  priceScale?: number   // divisor to convert fixed-point → display price
}

export function DepthChart({ bids, asks, priceScale = 1_000_000 }: Props) {
  const { bidPoints, askPoints, midPrice, spread } = useMemo(() => {
    let cum = 0
    const bp: [number, number][] = []
    for (const b of bids) {
      cum += b.qty
      bp.push([b.price / priceScale, cum])
    }
    cum = 0
    const ap: [number, number][] = []
    for (const a of asks) {
      cum += a.qty
      ap.push([a.price / priceScale, cum])
    }
    const bestBid = bids[0]?.price ?? 0
    const bestAsk = asks[0]?.price ?? 0
    const mid = bestBid && bestAsk ? (bestBid + bestAsk) / 2 / priceScale : 0
    const sp = bestBid && bestAsk ? (bestAsk - bestBid) / priceScale : 0
    return { bidPoints: bp, askPoints: ap, midPrice: mid, spread: sp }
  }, [bids, asks, priceScale])

  const option = useMemo(() => ({
    animation: true,
    animationDuration: 320,
    animationEasing: 'cubicOut',
    grid: { left: 60, right: 18, top: 36, bottom: 38 },
    backgroundColor: 'transparent',
    tooltip: {
      trigger: 'axis',
      ...TOOLTIP_BASE,
      axisPointer: CROSSHAIR,
    },
    legend: { ...LEGEND_BASE },
    xAxis: {
      type: 'value',
      scale: true,
      axisLine: AXIS_LINE_SUBTLE,
      axisLabel: { ...AXIS_LABEL, formatter: (v: number) => v.toFixed(2) },
      axisPointer: { label: { formatter: (p: { value: number }) => p.value.toFixed(4) } },
      splitLine: { show: false },
    },
    yAxis: {
      type: 'value',
      axisLine: { show: false },
      axisLabel: {
        ...AXIS_LABEL,
        formatter: (v: number) =>
          v >= 1e6 ? `${(v / 1e6).toFixed(1)}M`
            : v >= 1e3 ? `${(v / 1e3).toFixed(1)}k`
            : v.toFixed(0),
      },
      splitLine: SPLIT_LINE_SUBTLE,
    },
    series: [
      {
        name: 'bids',
        type: 'line',
        step: 'end',
        showSymbol: false,
        smooth: false,
        sampling: 'lttb',
        lineStyle: glowLine(CHART_COLORS.live, 2, 12),
        areaStyle: { color: areaGradient('rgba(16,240,149,0.32)', 'rgba(16,240,149,0.02)') },
        data: bidPoints,
        // Vertical mid-price marker drawn once on the bid series.
        markLine: midPrice
          ? {
              symbol: 'none',
              silent: true,
              label: {
                color: '#9ba0a6',
                fontFamily: 'var(--font-mono, monospace)',
                fontSize: 9,
                formatter: 'mid',
                position: 'insideEndTop',
              },
              lineStyle: { color: 'rgba(255,255,255,0.25)', type: 'dashed', width: 1 },
              data: [{ xAxis: midPrice }],
            }
          : undefined,
      },
      {
        name: 'asks',
        type: 'line',
        step: 'start',
        showSymbol: false,
        smooth: false,
        sampling: 'lttb',
        lineStyle: glowLine(CHART_COLORS.ask, 2, 12),
        areaStyle: { color: areaGradient('rgba(244,63,94,0.32)', 'rgba(244,63,94,0.02)') },
        data: askPoints,
      },
    ],
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }), [bidPoints, askPoints, midPrice])

  return (
    <div className="space-y-2">
      <div className="flex items-baseline gap-6 font-mono text-2xs uppercase tracking-widest text-muted-foreground">
        <div>
          mid <span className="ml-2 text-foreground">
            {midPrice ? midPrice.toFixed(4) : '—'}
          </span>
        </div>
        <div>
          spread <span className="ml-2 text-foreground">
            {spread ? spread.toFixed(4) : '—'}
          </span>
        </div>
        <div>
          bid depth <span className="ml-2 text-foreground">
            {(bidPoints[bidPoints.length - 1]?.[1] ?? 0).toLocaleString()}
          </span>
        </div>
        <div>
          ask depth <span className="ml-2 text-foreground">
            {(askPoints[askPoints.length - 1]?.[1] ?? 0).toLocaleString()}
          </span>
        </div>
      </div>
      <div className="h-72 w-full">
        <ReactECharts
          option={option}
          notMerge
          lazyUpdate
          style={{ height: '100%', width: '100%' }}
        />
      </div>
    </div>
  )
}
