/**
 * <RoleGate cap="chaos:inject">…</RoleGate>
 *
 * Capability-based UI gating. Wrap any control that should disappear
 * for users who don't have the required capability. The component
 * renders nothing (returns null) when the user lacks the cap — there's
 * no fallback prop because UI controls that are "greyed out for
 * permission reasons" tend to confuse rather than inform. If a
 * disabled-but-visible state is genuinely needed, use the explicit
 * `<RoleGate cap="…" fallback={…}>` form.
 *
 * IMPORTANT: this is a *UX convenience*, not a security boundary. The
 * gateway enforces the same capability check on every request; even a
 * crafted browser session with the localStorage flag flipped will get
 * a 403.
 */

'use client'

import type { ReactNode } from 'react'

import { useSession } from '@/lib/auth/session'

type Props = {
  cap: string
  children: ReactNode
  fallback?: ReactNode
}

export function RoleGate({ cap, children, fallback = null }: Props) {
  const { can, session } = useSession()
  if (!session) return null
  return can(cap) ? <>{children}</> : <>{fallback}</>
}
