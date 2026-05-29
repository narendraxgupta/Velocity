/**
 * Lightweight client-side session model. Mirrors the gateway's
 * /v1/auth/me response. The whole point of this module is to keep the
 * "what can this user do" question in ONE place, so components don't
 * each grow their own role-checking logic.
 *
 * Capabilities (the source of truth) come from the gateway. We don't
 * try to second-guess them in JS — even the role string is only used
 * for display. If you find yourself writing `if (role === 'admin')`,
 * stop and add the capability you actually need to the gateway's
 * /v1/auth/me bundle.
 */

import { useCallback, useEffect, useState } from 'react'

const API_BASE =
  process.env.NEXT_PUBLIC_API_GATEWAY_URL ?? 'http://localhost:8080'

export type Role = 'submitter' | 'operator' | 'admin'

export type Session = {
  tenant_id: string
  subject: string
  role: Role
  capabilities: string[]
}

const ANONYMOUS: Session = {
  tenant_id: 'default',
  subject: 'anonymous',
  role: 'submitter',
  capabilities: ['submissions:read', 'leaderboard:read', 'marketdata:read'],
}

const TOKEN_KEY = 'velocity.jwt'

export function getStoredToken(): string | null {
  if (typeof window === 'undefined') return null
  return window.localStorage.getItem(TOKEN_KEY)
}

export function setStoredToken(token: string | null): void {
  if (typeof window === 'undefined') return
  if (token == null) window.localStorage.removeItem(TOKEN_KEY)
  else window.localStorage.setItem(TOKEN_KEY, token)
}

export async function fetchSession(token?: string | null): Promise<Session> {
  const headers: Record<string, string> = { Accept: 'application/json' }
  const tok = token ?? getStoredToken()
  if (tok) headers.Authorization = `Bearer ${tok}`
  const r = await fetch(`${API_BASE}/v1/auth/me`, { headers, cache: 'no-store' })
  if (r.status === 401) return ANONYMOUS
  if (!r.ok) throw new Error(`auth/me HTTP ${r.status}`)
  return (await r.json()) as Session
}

/**
 * useSession — hydrates the session once on mount, exposes the helper
 * `can(cap)` for capability checks, and `login(token)` / `logout()` for
 * the token bookkeeping. Tightly scoped on purpose — we don't want a
 * 300-line auth provider for a 30-line need.
 */
export function useSession() {
  const [session, setSession] = useState<Session | null>(null)
  const [error, setError] = useState<Error | null>(null)

  const refresh = useCallback(async (token?: string | null) => {
    try {
      setError(null)
      setSession(await fetchSession(token))
    } catch (e) {
      setError(e as Error)
      setSession(ANONYMOUS)
    }
  }, [])

  useEffect(() => {
    void refresh()
  }, [refresh])

  const can = useCallback(
    (cap: string) => session?.capabilities?.includes(cap) ?? false,
    [session],
  )

  const login = useCallback(
    async (token: string) => {
      setStoredToken(token)
      await refresh(token)
    },
    [refresh],
  )

  const logout = useCallback(async () => {
    setStoredToken(null)
    await refresh(null)
  }, [refresh])

  return { session, error, can, login, logout, refresh }
}
