/**
 * =============================================================================
 *  Velocity — Root Layout
 *
 *  Sets up the global font stack (Geist Sans + Geist Mono), the theme
 *  metadata, and the persistent top navigation. Every page in the app is
 *  rendered inside this shell.
 * =============================================================================
 */

import type { Metadata, Viewport } from 'next'
import { GeistSans } from 'geist/font/sans'
import { GeistMono } from 'geist/font/mono'
import { Toaster } from 'sonner'

import { OnboardingWizard } from '@/components/onboarding/onboarding-wizard'
import { TopNav } from '@/components/top-nav'
import { TickerStrip } from '@/components/ticker-strip'
import { CommandPalette } from '@/components/ui/command-palette'
import { ThemeProvider } from '@/components/ui/theme-provider'
import { cn } from '@/lib/utils'

import './globals.css'

export const metadata: Metadata = {
  title: {
    default: 'Velocity — Trading Infrastructure Benchmark',
    template: '%s · Velocity',
  },
  description:
    'A distributed benchmarking platform for trading infrastructure. Submit a matching engine; Velocity measures latency, throughput, and correctness under adversarial load.',
  applicationName: 'Velocity',
  authors: [{ name: 'Velocity' }],
  keywords: [
    'trading',
    'matching engine',
    'load testing',
    'benchmark',
    'low latency',
    'observability',
  ],
  metadataBase: new URL('http://localhost:3000'),
  openGraph: {
    title: 'Velocity — Trading Infrastructure Benchmark',
    description:
      'Architect-grade benchmarking for trading infrastructure. C++ on the hot path, gVisor sandboxes, microsecond-honest latency.',
    type: 'website',
  },
}

export const viewport: Viewport = {
  themeColor: '#0A0A0B',
  width: 'device-width',
  initialScale: 1,
  maximumScale: 1,
}

export default function RootLayout({
  children,
}: {
  children: React.ReactNode
}) {
  return (
    <html
      lang="en"
      suppressHydrationWarning
      className={cn(GeistSans.variable, GeistMono.variable)}
    >
      <body className="min-h-screen flex flex-col bg-background text-foreground">
        <ThemeProvider>
          <TopNav />
          <TickerStrip />
          <main className="flex-1">{children}</main>
          <CommandPalette />
          {/* Shown once on first visit; gated by localStorage. Public
              share pages live under /share/<token> and are rendered
              standalone outside this chrome — so the wizard doesn't
              show up there even on first visit. */}
          <OnboardingWizard />
          <Toaster
            position="bottom-right"
            theme="dark"
            // `unstyled` strips Sonner's built-in chrome so our classes are
            // the only source of truth — otherwise the default opaque card
            // bleeds through and defeats the glass blur.
            toastOptions={{
              unstyled: true,
              classNames: {
                toast:
                  'group pointer-events-auto flex w-full items-start gap-3 rounded-lg p-4 text-foreground glass-soft',
                title:       'text-sm font-semibold tracking-tight',
                description: 'text-xs text-muted-foreground',
                actionButton:
                  'rounded border border-accent/30 bg-accent/15 px-2 py-1 text-2xs font-medium uppercase tracking-widest text-accent hover:bg-accent/25',
                cancelButton:
                  'rounded border border-white/[0.08] bg-white/[0.04] px-2 py-1 text-2xs font-medium uppercase tracking-widest text-muted-foreground hover:bg-white/[0.08]',
                closeButton: 'text-muted-foreground hover:text-foreground',
                // Variant accents — left edge gets a 2px coloured stripe to
                // convey severity at a glance without breaking the glass.
                success:
                  'shadow-[inset_2px_0_0_0_hsl(var(--signal-live)),0_12px_28px_-10px_rgb(0_0_0/0.55)]',
                error:
                  'shadow-[inset_2px_0_0_0_hsl(var(--signal-ask)),0_12px_28px_-10px_rgb(0_0_0/0.55)]',
                warning:
                  'shadow-[inset_2px_0_0_0_hsl(var(--signal-warn)),0_12px_28px_-10px_rgb(0_0_0/0.55)]',
                info:
                  'shadow-[inset_2px_0_0_0_hsl(var(--signal-info)),0_12px_28px_-10px_rgb(0_0_0/0.55)]',
              },
            }}
          />
        </ThemeProvider>
      </body>
    </html>
  )
}
