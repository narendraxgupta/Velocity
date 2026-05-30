/**
 * Single source of truth for the API-gateway base URL.
 *
 * Browser → returns '' (empty), so every gateway call is SAME-ORIGIN
 * (`/v1/...` on the page's own host/port). The Next.js server then proxies
 * `/v1/*` to the gateway over the internal network (see next.config.mjs
 * `rewrites`).
 *
 * Why: behind the GitHub Codespaces tunnel, a cross-origin call from the
 * `*-3000` host to the `*-8080` host triggers a CORS preflight (`OPTIONS`)
 * that the tunnel rejects with 403 *before it ever reaches the gateway* — so
 * the gateway's own CORS handling never runs. Going same-origin removes the
 * preflight entirely and means port 8080 no longer needs to be public.
 *
 * Escape hatch: set NEXT_PUBLIC_API_GATEWAY_URL to a non-empty absolute URL
 * only if you deliberately want the browser to hit the gateway cross-origin
 * (and have CORS + a public port configured for it).
 *
 * Server (RSC / route handlers) → relative URLs are invalid there, so we hit
 * the gateway directly over the internal Docker/cluster network.
 */
export function resolveApiBase(): string {
  if (typeof window !== 'undefined') {
    return process.env.NEXT_PUBLIC_API_GATEWAY_URL || ''
  }
  return (
    process.env.INTERNAL_API_GATEWAY_URL ||
    process.env.API_GATEWAY_ORIGIN ||
    process.env.NEXT_PUBLIC_API_GATEWAY_URL ||
    'http://api-gateway:8080'
  )
}

export const API_BASE = resolveApiBase()
