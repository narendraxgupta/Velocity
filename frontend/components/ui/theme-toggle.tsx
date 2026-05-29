/**
 * ThemeToggle — small icon button that flips dark <-> light.
 *
 * Uses next-themes for persistence. Renders nothing until mounted so the
 * server-rendered icon doesn't flicker.
 */

'use client'

import { Moon, Sun } from 'lucide-react'
import { useTheme } from 'next-themes'
import { useEffect, useState } from 'react'

import { cn } from '@/lib/utils'

export function ThemeToggle({ className }: { className?: string }) {
  const { theme, setTheme } = useTheme()
  const [mounted, setMounted] = useState(false)
  useEffect(() => setMounted(true), [])

  if (!mounted) {
    return (
      <span
        className={cn(
          'inline-flex h-7 w-7 items-center justify-center rounded border border-border-subtle bg-surface',
          className,
        )}
        aria-hidden
      />
    )
  }

  const next = theme === 'dark' ? 'light' : 'dark'
  return (
    <button
      type="button"
      onClick={() => setTheme(next)}
      aria-label={`Switch to ${next} theme`}
      title={`Switch to ${next} theme`}
      className={cn(
        'inline-flex h-7 w-7 items-center justify-center rounded border border-border-subtle bg-surface text-muted-foreground transition-colors hover:bg-surface-elevated hover:text-foreground',
        className,
      )}
    >
      {theme === 'dark' ? <Sun className="h-3.5 w-3.5" /> : <Moon className="h-3.5 w-3.5" />}
    </button>
  )
}
