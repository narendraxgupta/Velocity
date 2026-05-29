/**
 * ThemeProvider — wraps next-themes with our defaults.
 *
 * We default to the dark "terminal" theme; light is an opt-in accessibility
 * variant. CSS variables in `globals.css` switch based on the `data-theme`
 * attribute that next-themes sets on `<html>`.
 */

'use client'

import { ThemeProvider as NextThemesProvider } from 'next-themes'
import type { ThemeProviderProps } from 'next-themes/dist/types'

export function ThemeProvider({ children, ...props }: ThemeProviderProps) {
  return (
    <NextThemesProvider
      attribute="data-theme"
      defaultTheme="dark"
      enableSystem={false}
      disableTransitionOnChange
      {...props}
    >
      {children}
    </NextThemesProvider>
  )
}
