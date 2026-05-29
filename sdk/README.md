# Velocity SDKs

Three first-party SDKs for the Velocity HTTP + gRPC surface.
Source-of-truth wire contracts live in `proto/`. The SDKs wrap those
contracts in language-idiomatic clients so callers don't have to
hand-roll fetch+JSON.

| Language   | Path           | Registry                                            |
|------------|----------------|-----------------------------------------------------|
| Go         | `sdk/go/`      | Imported direct from this repo at a tag             |
| TypeScript | `sdk/ts/`      | GitHub Packages (`npm.pkg.github.com`) — `@velocity/sdk` |
| Python     | `sdk/python/`  | GitHub Packages (PyPI-style)                       — `velocity-sdk`   |

## Releasing

```bash
make sdk-release VERSION=0.1.2
git push origin sdk-v0.1.2
```

The `release-sdk` GitHub Actions workflow takes over from there:

1. Regenerates proto stubs with `buf generate`.
2. Smoke-builds the Go SDK (consumers fetch direct from the tag, so
   nothing else to publish there).
3. Builds + publishes the TS package to GitHub Packages.
4. Builds + publishes the Python wheel + sdist to GitHub Packages.

Each language is its own job; one failure does not block the others.

## Coding conventions

All three SDKs share:

- A `Client` (or `VelocityClient`) entry point with `baseUrl` + `bearer`.
- Resource sub-clients (`submissions`, `benchmarks`, `leaderboard`, `audit`).
- A streaming primitive (`watch()` / `stream()`) that parses SSE frames.
- Capped-exponential retry on 5xx + transport errors (max 2 retries).
- Errors split into "API error" (status + body) and "network error".

Pull requests adding a new resource should add it to *all three* SDKs
in lockstep — the symmetry is the point.
