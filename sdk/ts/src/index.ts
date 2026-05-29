/**
 * @velocity/sdk — TypeScript client for the Velocity platform.
 *
 * The exported `VelocityClient` is the main entry point. Resource
 * sub-clients (`client.submissions`, `client.benchmarks`, …) host the
 * per-resource methods.
 *
 * Design notes:
 *   - Native `fetch` only; no axios. The SDK runs unchanged in modern
 *     Node, Deno, Bun, and the browser.
 *   - Streaming (`watch`) is implemented as an async generator that
 *     consumes Server-Sent Events. We do NOT use EventSource because
 *     it can't send Authorization headers and it doesn't surface
 *     server-side errors cleanly.
 *   - Errors are `VelocityApiError` with HTTP status + body. The
 *     status is exposed so callers can branch on 401 / 403 / 5xx.
 */

export * from './errors.js'
export * from './client.js'
export * from './submissions.js'
export * from './benchmarks.js'
export * from './leaderboard.js'
export * from './audit.js'
