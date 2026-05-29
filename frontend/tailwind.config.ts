/**
 * =============================================================================
 *  Velocity — Tailwind theme: Bloomberg × Linear Hybrid
 * =============================================================================
 *
 *  Design philosophy
 *  -----------------
 *  Trading terminal density (Bloomberg) layered with modern SaaS polish
 *  (Linear / Vercel). The result reads as "Bloomberg from 2026, built by
 *  people who care about whitespace."
 *
 *  Palette
 *  -------
 *    Background     near-black with cool tint  (#0A0A0B)
 *    Surface        elevated panels             (#101013, #16161A)
 *    Border         subtle, low contrast        (#1F1F25)
 *    Foreground     warm-white                  (#E6E6EA)
 *    Muted          dimmed labels               (#7B7B85)
 *
 *  Signal colors (used sparingly — these are *meaning*, not decoration)
 *    Live (electric green)   #10F095   →  data is updating right now
 *    Bid                     #10F095
 *    Ask (warm red)          #F43F5E
 *    Warn (amber)            #FFA940
 *    Info (cyan)             #22D3EE
 *    Accent (violet, brand)  #7C66FF
 *
 *  Typography
 *  ----------
 *    UI chrome              Geist Sans     (system font where available)
 *    Numbers / tickers      Geist Mono     (tabular-nums, slashed-zero)
 *    Headlines              Geist Sans Bold
 *
 *  Motion
 *  ------
 *  All animations are subtle. The leaderboard reorders with
 *  spring(stiffness=300, damping=30). Live-update flashes use a 600ms
 *  green→neutral keyframe. No bouncy easing.
 * =============================================================================
 */

import type { Config } from 'tailwindcss'
import animate from 'tailwindcss-animate'

const config = {
  darkMode: ['class'],
  content: [
    './app/**/*.{ts,tsx}',
    './components/**/*.{ts,tsx}',
    './lib/**/*.{ts,tsx}',
  ],
  theme: {
    container: {
      center: true,
      padding: '1.5rem',
      screens: {
        '2xl': '1440px',
      },
    },
    extend: {
      colors: {
        background: 'hsl(var(--background) / <alpha-value>)',
        foreground: 'hsl(var(--foreground) / <alpha-value>)',
        surface: {
          DEFAULT: 'hsl(var(--surface) / <alpha-value>)',
          elevated: 'hsl(var(--surface-elevated) / <alpha-value>)',
          subtle: 'hsl(var(--surface-subtle) / <alpha-value>)',
        },
        border: {
          DEFAULT: 'hsl(var(--border) / <alpha-value>)',
          subtle: 'hsl(var(--border-subtle) / <alpha-value>)',
        },
        muted: {
          DEFAULT: 'hsl(var(--muted) / <alpha-value>)',
          foreground: 'hsl(var(--muted-foreground) / <alpha-value>)',
        },
        signal: {
          live: 'hsl(var(--signal-live) / <alpha-value>)',
          bid: 'hsl(var(--signal-bid) / <alpha-value>)',
          ask: 'hsl(var(--signal-ask) / <alpha-value>)',
          warn: 'hsl(var(--signal-warn) / <alpha-value>)',
          info: 'hsl(var(--signal-info) / <alpha-value>)',
          neutral: 'hsl(var(--signal-neutral) / <alpha-value>)',
        },
        accent: {
          DEFAULT: 'hsl(var(--accent) / <alpha-value>)',
          foreground: 'hsl(var(--accent-foreground) / <alpha-value>)',
          subtle: 'hsl(var(--accent-subtle) / <alpha-value>)',
        },
        destructive: {
          DEFAULT: 'hsl(var(--destructive) / <alpha-value>)',
          foreground: 'hsl(var(--destructive-foreground) / <alpha-value>)',
        },
        success: 'hsl(var(--success) / <alpha-value>)',
        warning: 'hsl(var(--warning) / <alpha-value>)',
        info: 'hsl(var(--info) / <alpha-value>)',
        chart: {
          1: 'hsl(var(--chart-1) / <alpha-value>)',
          2: 'hsl(var(--chart-2) / <alpha-value>)',
          3: 'hsl(var(--chart-3) / <alpha-value>)',
          4: 'hsl(var(--chart-4) / <alpha-value>)',
          5: 'hsl(var(--chart-5) / <alpha-value>)',
          6: 'hsl(var(--chart-6) / <alpha-value>)',
        },
      },
      fontFamily: {
        sans: [
          'var(--font-geist-sans)',
          'Inter',
          'system-ui',
          '-apple-system',
          'BlinkMacSystemFont',
          'Segoe UI',
          'sans-serif',
        ],
        mono: [
          'var(--font-geist-mono)',
          'JetBrains Mono',
          'SF Mono',
          'Menlo',
          'Consolas',
          'monospace',
        ],
        display: [
          'var(--font-geist-sans)',
          'Inter',
          'system-ui',
          'sans-serif',
        ],
      },
      fontSize: {
        '2xs': ['0.6875rem', { lineHeight: '0.875rem', letterSpacing: '0.02em' }],
        xs: ['0.75rem', { lineHeight: '1rem' }],
        sm: ['0.8125rem', { lineHeight: '1.125rem' }],
        base: ['0.875rem', { lineHeight: '1.25rem' }],
        md: ['0.9375rem', { lineHeight: '1.375rem' }],
        lg: ['1rem', { lineHeight: '1.5rem' }],
        xl: ['1.125rem', { lineHeight: '1.625rem' }],
        '2xl': ['1.375rem', { lineHeight: '1.875rem', letterSpacing: '-0.01em' }],
        '3xl': ['1.75rem', { lineHeight: '2.25rem', letterSpacing: '-0.02em' }],
        '4xl': ['2.25rem', { lineHeight: '2.75rem', letterSpacing: '-0.025em' }],
        '5xl': ['3rem', { lineHeight: '3.25rem', letterSpacing: '-0.03em' }],
      },
      letterSpacing: {
        tighter: '-0.025em',
        tight: '-0.015em',
        normal: '0',
        wide: '0.01em',
        wider: '0.025em',
        widest: '0.08em',
      },
      spacing: {
        '0.75': '0.1875rem',
        '1.25': '0.3125rem',
        '1.75': '0.4375rem',
        '4.5': '1.125rem',
        '5.5': '1.375rem',
        '6.5': '1.625rem',
        '13': '3.25rem',
        '15': '3.75rem',
        '18': '4.5rem',
      },
      borderRadius: {
        none: '0',
        xs: '2px',
        sm: '3px',
        DEFAULT: '4px',
        md: '6px',
        lg: '8px',
        xl: '12px',
        '2xl': '16px',
      },
      boxShadow: {
        'glow-live': '0 0 0 1px hsl(var(--signal-live) / 0.4), 0 0 18px -4px hsl(var(--signal-live) / 0.35)',
        'glow-ask':  '0 0 0 1px hsl(var(--signal-ask)  / 0.4), 0 0 18px -4px hsl(var(--signal-ask)  / 0.35)',
        'glow-accent': '0 0 0 1px hsl(var(--accent) / 0.5), 0 0 24px -6px hsl(var(--accent) / 0.4)',
        'inset-border': 'inset 0 0 0 1px hsl(var(--border) / 1)',
        'panel-sm': '0 1px 2px 0 rgb(0 0 0 / 0.5)',
        'panel-md': '0 4px 12px -2px rgb(0 0 0 / 0.6), 0 2px 4px -1px rgb(0 0 0 / 0.4)',
        'panel-lg': '0 12px 32px -8px rgb(0 0 0 / 0.7), 0 4px 12px -4px rgb(0 0 0 / 0.5)',
      },
      keyframes: {
        'pulse-live': {
          '0%, 100%': { opacity: '1', transform: 'scale(1)' },
          '50%':      { opacity: '0.55', transform: 'scale(1.4)' },
        },
        'flash-up': {
          '0%':   { backgroundColor: 'hsl(var(--signal-live) / 0.25)' },
          '100%': { backgroundColor: 'transparent' },
        },
        'flash-down': {
          '0%':   { backgroundColor: 'hsl(var(--signal-ask) / 0.25)' },
          '100%': { backgroundColor: 'transparent' },
        },
        'ticker-scroll': {
          '0%':   { transform: 'translateX(0)' },
          '100%': { transform: 'translateX(-100%)' },
        },
        'shimmer': {
          '0%':   { backgroundPosition: '-200% 0' },
          '100%': { backgroundPosition: '200% 0' },
        },
        'fade-in-up': {
          '0%':   { opacity: '0', transform: 'translateY(6px)' },
          '100%': { opacity: '1', transform: 'translateY(0)' },
        },
      },
      animation: {
        'pulse-live':    'pulse-live 1.6s cubic-bezier(0.4, 0, 0.6, 1) infinite',
        'flash-up':      'flash-up 600ms ease-out forwards',
        'flash-down':    'flash-down 600ms ease-out forwards',
        'ticker-scroll': 'ticker-scroll 60s linear infinite',
        'shimmer':       'shimmer 1.8s linear infinite',
        'fade-in-up':    'fade-in-up 0.25s ease-out forwards',
      },
      backgroundImage: {
        'grid-fine':
          'linear-gradient(to right, hsl(var(--border-subtle) / 0.5) 1px, transparent 1px), linear-gradient(to bottom, hsl(var(--border-subtle) / 0.5) 1px, transparent 1px)',
        'gradient-radial':
          'radial-gradient(ellipse at top, hsl(var(--accent) / 0.08), transparent 70%)',
        'gradient-hero':
          'radial-gradient(ellipse 80% 50% at 50% -20%, hsl(var(--accent) / 0.15), transparent), linear-gradient(180deg, hsl(var(--background)), hsl(var(--surface) / 1))',
      },
      backgroundSize: {
        'grid-fine': '32px 32px',
        'shimmer': '200% 100%',
      },
    },
  },
  plugins: [animate],
} satisfies Config

export default config
