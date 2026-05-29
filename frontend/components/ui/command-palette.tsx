/**
 * CommandPalette — Ctrl+K launcher.
 *
 * Keyboard-driven navigation across every primary screen plus the most common
 * in-app actions (start benchmark, jump to admin, toggle demo mode).
 *
 * The key handler also accepts Cmd+K so the same binary works on a Mac if you
 * ever cross-compile/run there, but the on-screen hint is Ctrl+K because the
 * canonical deployment target is Windows / Linux.
 *
 * Visual treatment
 * ----------------
 * Glassmorphism — a frosted panel sitting above a blurred page scrim. The
 * panel uses the `.glass-panel` design-system class so future modals
 * (settings, confirmations, etc.) get the same treatment by reusing it.
 *
 * Composition notes
 * -----------------
 *   - Radix Dialog primitives drive open/close, focus trap, ESC handling and
 *     the overlay. We do NOT use `Command.Dialog` from cmdk because that
 *     wrapper does not let us style the overlay independently.
 *   - The `Command` root from cmdk renders inside `Dialog.Content` so we keep
 *     all of cmdk's keyboard navigation and fuzzy filtering.
 *   - Page list is statically declared here so the palette works without a
 *     backend. Live commands post to the gateway directly.
 */

'use client'

import * as Dialog from '@radix-ui/react-dialog'
import { Command } from 'cmdk'
import {
  ActivitySquare,
  ChevronRight,
  GitCompareArrows,
  Hammer,
  LayoutDashboard,
  Moon,
  PlayCircle,
  ScrollText,
  Sparkles,
  Sun,
  Trophy,
  Upload,
} from 'lucide-react'
import { useRouter } from 'next/navigation'
import { useTheme } from 'next-themes'
import { useCallback, useEffect, useState } from 'react'
import { toast } from 'sonner'

import { cn } from '@/lib/utils'

export function CommandPalette() {
  const router = useRouter()
  const { theme, setTheme } = useTheme()
  const [open, setOpen] = useState(false)

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && (e.key === 'k' || e.key === 'K')) {
        e.preventDefault()
        setOpen((v) => !v)
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  const go = useCallback(
    (path: string) => {
      setOpen(false)
      router.push(path)
    },
    [router],
  )

  const startBaseline = useCallback(() => {
    setOpen(false)
    toast('Pick a submission in Admin', {
      description: 'Benchmarks require a submission_id — open the operator console to start one.',
      action: {
        label: 'Open Admin',
        onClick: () => router.push('/admin'),
      },
    })
  }, [router])

  const toggleDemo = useCallback(() => {
    setOpen(false)
    if (typeof window === 'undefined') return
    const cur = window.localStorage.getItem('velocity:demo') === '1'
    if (cur) {
      window.localStorage.removeItem('velocity:demo')
      toast('Demo mode disabled — reload to see live data')
    } else {
      window.localStorage.setItem('velocity:demo', '1')
      toast.success('Demo mode enabled — reload to see canned data')
    }
  }, [])

  return (
    <Dialog.Root open={open} onOpenChange={setOpen}>
      <Dialog.Portal>
        <Dialog.Overlay
          className={cn(
            'fixed inset-0 z-50 glass-scrim',
            'data-[state=open]:animate-in data-[state=open]:fade-in-0',
            'data-[state=closed]:animate-out data-[state=closed]:fade-out-0',
            'duration-200',
          )}
        />
        <Dialog.Content
          aria-label="Command menu"
          className={cn(
            // Position — slightly above center so the eyeline lands on the input.
            'fixed left-1/2 top-[18%] z-[60] w-[36rem] max-w-[92vw] -translate-x-1/2',
            // Glass shell — translucent surface + frosted blur + rim highlight.
            'rounded-xl overflow-hidden glass-panel',
            // Animations — softer than a hard pop; matches Linear / cmd-k feel.
            'data-[state=open]:animate-in data-[state=open]:fade-in-0 data-[state=open]:zoom-in-95 data-[state=open]:slide-in-from-top-2',
            'data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=closed]:zoom-out-95',
            'duration-200 ease-out',
          )}
        >
          {/* Screen-reader title — visually hidden but required for a11y by Radix. */}
          <Dialog.Title className="sr-only">Command palette</Dialog.Title>
          <Dialog.Description className="sr-only">
            Jump to screens, start benchmarks, or change the theme.
          </Dialog.Description>

          <Command
            label="Command menu"
            filter={(value, search) =>
              value.toLowerCase().includes(search.toLowerCase()) ? 1 : 0
            }
            className="relative z-[1]"
          >
            <div className="flex items-center gap-2 border-b border-white/[0.06] px-3 py-2.5">
              <Sparkles className="h-4 w-4 text-accent/80" />
              <Command.Input
                autoFocus
                placeholder="Jump to a screen, start a benchmark, toggle the theme…"
                className="flex-1 bg-transparent text-sm placeholder:text-muted-foreground focus:outline-none"
              />
              <kbd className="rounded border border-white/[0.08] bg-white/[0.04] px-1.5 py-0.5 font-mono text-2xs uppercase tracking-widest text-muted-foreground">
                esc
              </kbd>
            </div>
            <Command.List className="max-h-[24rem] overflow-y-auto p-2">
              <Command.Empty className="px-3 py-6 text-center text-sm text-muted-foreground">
                Nothing matches. Try “leaderboard” or “fleet”.
              </Command.Empty>

              <Group heading="Navigate">
                <Item icon={<LayoutDashboard className="h-4 w-4" />} onSelect={() => go('/')}             label="Overview" />
                <Item icon={<Trophy className="h-4 w-4" />}            onSelect={() => go('/leaderboard')} label="Leaderboard" />
                <Item icon={<Upload className="h-4 w-4" />}            onSelect={() => go('/submissions')} label="Submissions" />
                <Item icon={<ActivitySquare className="h-4 w-4" />}    onSelect={() => go('/fleet')}       label="Bot fleet" />
                <Item icon={<GitCompareArrows className="h-4 w-4" />}  onSelect={() => go('/compare')}     label="Compare runs" />
                <Item icon={<Hammer className="h-4 w-4" />}            onSelect={() => go('/admin')}       label="Admin console" />
              </Group>

              <Group heading="Actions">
                <Item icon={<PlayCircle className="h-4 w-4" />} onSelect={startBaseline}
                      label="Start a 60s baseline benchmark" hint="POSTs /v1/benchmarks" />
                <Item
                  icon={theme === 'dark' ? <Sun className="h-4 w-4" /> : <Moon className="h-4 w-4" />}
                  onSelect={() => { setTheme(theme === 'dark' ? 'light' : 'dark'); setOpen(false) }}
                  label={`Switch to ${theme === 'dark' ? 'light' : 'dark'} theme`}
                />
                <Item icon={<Sparkles className="h-4 w-4" />} onSelect={toggleDemo}
                      label="Toggle demo mode" hint="localStorage flag — reload to apply" />
                <Item icon={<ScrollText className="h-4 w-4" />} onSelect={() => go('/admin#runs')}
                      label="View recent run history" />
              </Group>
            </Command.List>
          </Command>
        </Dialog.Content>
      </Dialog.Portal>
    </Dialog.Root>
  )
}

/* -------------------------------------------------------------------------- */

function Group({ heading, children }: { heading: string; children: React.ReactNode }) {
  return (
    <Command.Group
      heading={heading}
      className="[&_[cmdk-group-heading]]:label-eyebrow [&_[cmdk-group-heading]]:px-2 [&_[cmdk-group-heading]]:py-1"
    >
      {children}
    </Command.Group>
  )
}

function Item({
  icon,
  label,
  hint,
  onSelect,
}: {
  icon: React.ReactNode
  label: string
  hint?: string
  onSelect: () => void
}) {
  return (
    <Command.Item
      onSelect={onSelect}
      value={label}
      className={cn(
        'flex items-center gap-3 rounded-md px-2 py-2 text-sm cursor-pointer transition-colors',
        // Selected (keyboard) state — translucent accent fill + crisp ring.
        'aria-selected:bg-white/[0.06] aria-selected:text-foreground aria-selected:shadow-[inset_0_0_0_1px_hsl(var(--accent)/0.18)]',
        'hover:bg-white/[0.04]',
      )}
    >
      <span className="text-muted-foreground">{icon}</span>
      <span className="flex-1">{label}</span>
      {hint ? (
        <span className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
          {hint}
        </span>
      ) : (
        <ChevronRight className="h-3.5 w-3.5 text-muted-foreground" />
      )}
    </Command.Item>
  )
}
