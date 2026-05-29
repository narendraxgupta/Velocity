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
    animation: false,
    grid: { left: 56, right: 16, top: 32, bottom: 36 },
    backgroundColor: 'transparent',
    tooltip: {
      trigger: 'axis',
      backgroundColor: 'rgba(15,16,20,0.94)',
      borderColor: 'rgba(255,255,255,0.06)',
      borderWidth: 1,
      textStyle: { color: '#e6e7ea', fontFamily: 'var(--font-mono, monospace)', fontSize: 12 },
    },
    legend: {
      bottom: 0,
      textStyle: { color: '#9ba0a6', fontFamily: 'var(--font-mono, monospace)', fontSize: 10 },
      itemWidth: 12,
      itemHeight: 2,
    },
    xAxis: {
      type: 'value',
      scale: true,
      axisLine: { lineStyle: { color: 'rgba(255,255,255,0.06)' } },
      axisLabel: {
        color: '#6b7077',
        fontFamily: 'var(--font-mono, monospace)',
        fontSize: 10,
        formatter: (v: number) => v.toFixed(2),
      },
      splitLine: { show: false },
    },
    yAxis: {
      type: 'value',
      axisLine: { show: false },
      axisLabel: {
        color: '#6b7077',
        fontFamily: 'var(--font-mono, monospace)',
        fontSize: 10,
        formatter: (v: number) =>
          v >= 1e6 ? `${(v / 1e6).toFixed(1)}M`
            : v >= 1e3 ? `${(v / 1e3).toFixed(1)}k`
            : v.toFixed(0),
      },
      splitLine: { lineStyle: { color: 'rgba(255,255,255,0.04)' } },
    },
    series: [
      {
        name: 'bids',
        type: 'line',
        step: 'end',
        showSymbol: false,
        smooth: false,
        sampling: 'lttb',
        lineStyle: { width: 1.5, color: '#34d399' },
        areaStyle: { color: 'rgba(52,211,153,0.12)' },
        data: bidPoints,
      },
      {
        name: 'asks',
        type: 'line',
        step: 'start',
        showSymbol: false,
        smooth: false,
        sampling: 'lttb',
        lineStyle: { width: 1.5, color: '#f87171' },
        areaStyle: { color: 'rgba(248,113,113,0.12)' },
        data: askPoints,
      },
    ],
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }), [bidPoints, askPoints])

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
