/**
 * `apiFetch` — the one and only place we attach the JWT to outbound
 * requests from the browser. Use it instead of bare `fetch()` whenever
 * you are talking to the gateway.
 *
 * Why a module function and not a fetch interceptor?
 *
 *   - Next.js Server Components can't share state with the browser
 *     globals, so a global `fetch` patch would only work on the client.
 *   - We want this to be tree-shakable: code paths that don't talk to
 *     the gateway shouldn't pull the localStorage token logic in.
 *
 * If you need to make an unauthenticated call (e.g. to a third-party
 * URL), just keep using `fetch` directly.
 */

import { getStoredToken } from '@/lib/auth/session'

const API_BASE =
  process.env.NEXT_PUBLIC_API_GATEWAY_URL ?? 'http://localhost:8080'

export type ApiFetchInit = RequestInit & {
  /** Override the bearer token. Useful for "preview as tenant X" flows. */
  bearer?: string | null
}

export async function apiFetch(
  path: string,
  init: ApiFetchInit = {},
): Promise<Response> {
  const url = path.startsWith('http') ? path : `${API_BASE}${path}`
  const headers = new Headers(init.headers ?? {})
  const token = init.bearer !== undefined ? init.bearer : getStoredToken()
  if (token) headers.set('Authorization', `Bearer ${token}`)
  if (!headers.has('Accept')) headers.set('Accept', 'application/json')
  return fetch(url, { ...init, headers })
}

export async function apiJson<T>(
  path: string,
  init: ApiFetchInit = {},
): Promise<T> {
  const r = await apiFetch(path, init)
  if (!r.ok) {
    let detail = ''
    try {
      detail = (await r.text()).slice(0, 256)
    } catch {
      // ignore
    }
    throw new Error(`HTTP ${r.status} ${r.statusText} — ${detail}`)
  }
  return (await r.json()) as T
}
