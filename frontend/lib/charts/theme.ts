/**
 * Shared ECharts styling primitives — a small "trading terminal" toolkit.
 *
 * The goal is a Bloomberg/TradingView feel on a near-black canvas: neon lines
 * that glow, gradient area fills that fade to nothing, and a snapping crosshair
 * with monospace value labels. Import these helpers into each chart's `option`
 * so the whole app shares one visual language.
 *
 * Colours are kept as literal rgb/hsl strings (not Tailwind classes) because
 * ECharts renders to canvas and can't read CSS variables at paint time.
 */

/** Brand / signal palette mirrored from globals.css + tailwind.config.ts. */
export const CHART_COLORS = {
  live: '#10F095', // electric green  — bids / sustained / "good"
  ask: '#F43F5E', // warm red        — asks / errors / "bad"
  cyan: '#22D3EE', // info            — p50 / baselines
  violet: '#7C66FF', // brand accent  — offered / secondary series
  amber: '#FFA940', // warn           — p99 / cliffs
  magenta: '#D26BFF',
} as const

/** Vertical gradient (top → bottom). Use for `areaStyle.color`. */
export function areaGradient(top: string, bottom = 'rgba(0,0,0,0)') {
  return {
    type: 'linear',
    x: 0,
    y: 0,
    x2: 0,
    y2: 1,
    colorStops: [
      { offset: 0, color: top },
      { offset: 1, color: bottom },
    ],
  } as const
}

/** Horizontal gradient (left → right). Handy for depth fills. */
export function areaGradientH(left: string, right = 'rgba(0,0,0,0)') {
  return {
    type: 'linear',
    x: 0,
    y: 0,
    x2: 1,
    y2: 0,
    colorStops: [
      { offset: 0, color: left },
      { offset: 1, color: right },
    ],
  } as const
}

/** A glowing line style — the signature trading-terminal look. */
export function glowLine(color: string, width = 2, blur = 12) {
  return {
    width,
    color,
    shadowBlur: blur,
    shadowColor: color,
    shadowOffsetY: 0,
  }
}

/** Snapping crosshair axis pointer with styled value labels. */
export const CROSSHAIR = {
  type: 'cross' as const,
  snap: true,
  lineStyle: { color: 'rgba(255,255,255,0.16)', width: 1, type: 'dashed' as const },
  crossStyle: { color: 'rgba(255,255,255,0.16)', width: 1, type: 'dashed' as const },
  label: {
    backgroundColor: 'rgba(12,13,16,0.96)',
    borderColor: 'rgba(255,255,255,0.10)',
    borderWidth: 1,
    color: '#e6e7ea',
    fontFamily: 'var(--font-mono, monospace)',
    fontSize: 10,
    padding: [3, 6],
  },
}

/** Frosted, accent-ringed tooltip shell shared by every chart. */
export const TOOLTIP_BASE = {
  backgroundColor: 'rgba(10,11,13,0.95)',
  borderColor: 'rgba(124,102,255,0.30)',
  borderWidth: 1,
  padding: [8, 12] as [number, number],
  extraCssText:
    'box-shadow: 0 10px 40px -10px rgba(0,0,0,0.85); backdrop-filter: blur(8px); border-radius: 8px;',
  textStyle: {
    color: '#e6e7ea',
    fontFamily: 'var(--font-mono, monospace)',
    fontSize: 12,
  },
}

export const AXIS_LABEL = {
  color: '#6b7077',
  fontFamily: 'var(--font-mono, monospace)',
  fontSize: 10,
}

export const AXIS_LINE_SUBTLE = { lineStyle: { color: 'rgba(255,255,255,0.07)' } }
export const SPLIT_LINE_SUBTLE = { lineStyle: { color: 'rgba(255,255,255,0.035)' } }

export const LEGEND_BASE = {
  bottom: 0,
  icon: 'roundRect' as const,
  textStyle: {
    color: '#9ba0a6',
    fontFamily: 'var(--font-mono, monospace)',
    fontSize: 10,
  },
  itemWidth: 14,
  itemHeight: 3,
  itemGap: 14,
}
