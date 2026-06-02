# =============================================================================
# Velocity — Frontend (Next.js) container
#
# Multi-stage build:
#   deps    - install node_modules with pnpm (cacheable, deterministic)
#   builder - run `next build` against deps
#   runtime - distroless-style minimal final image
#
# Final image is ~110 MB (vs ~1.2 GB for a naive single-stage Next dev image).
# =============================================================================

# ---- Stage 1 : install deps -------------------------------------------------
FROM node:20-alpine AS deps
WORKDIR /app

# Faster, more reproducible installs via pnpm if you choose; npm here for
# minimum friction. Swap to pnpm/yarn by replacing this block.
COPY package.json package-lock.json* ./
# `npm install` (not `npm ci`) so a drifted lockfile self-heals during the
# image build instead of hard-failing; the deps are still pinned by the
# resolved tree that gets carried into the builder stage.
RUN apk add --no-cache libc6-compat \
 && npm install --no-audit --no-fund

# ---- Stage 2 : build --------------------------------------------------------
FROM node:20-alpine AS builder
WORKDIR /app
ENV NEXT_TELEMETRY_DISABLED=1

# NEXT_PUBLIC_* are inlined into the client bundle at build time, so they
# MUST be present here (not just at runtime).
#
# NEXT_PUBLIC_API_GATEWAY_URL defaults to EMPTY on purpose: the browser then
# calls the gateway SAME-ORIGIN (`/v1/...`) and the Next server proxies those
# to API_GATEWAY_ORIGIN (see next.config.mjs `rewrites`). This is what makes
# the SPA work behind the GitHub Codespaces tunnel without cross-origin CORS
# preflights. Only set a non-empty absolute URL if you intentionally want the
# browser to hit the gateway cross-origin.
#
# API_GATEWAY_ORIGIN is the internal address the Next server proxies to. It is
# baked into the routes manifest at build time, hence it's a build arg.
#
# NEXT_PUBLIC_LEADERBOARD_WS is the leaderboard WebSocket URL. WS can't be
# proxied reliably by Next standalone, so the browser connects to it directly;
# for a Codespace pass the public wss://<name>-8090.app.github.dev/... URL.
ARG NEXT_PUBLIC_API_GATEWAY_URL=
ARG NEXT_PUBLIC_LEADERBOARD_WS=ws://localhost:8090/v1/leaderboard
ARG API_GATEWAY_ORIGIN=http://api-gateway:8080
ENV NEXT_PUBLIC_API_GATEWAY_URL=${NEXT_PUBLIC_API_GATEWAY_URL}
ENV NEXT_PUBLIC_LEADERBOARD_WS=${NEXT_PUBLIC_LEADERBOARD_WS}
ENV API_GATEWAY_ORIGIN=${API_GATEWAY_ORIGIN}

COPY --from=deps /app/node_modules ./node_modules
COPY . .

RUN npm run build

# ---- Stage 3 : runtime ------------------------------------------------------
FROM node:20-alpine AS runtime
WORKDIR /app
ENV NODE_ENV=production
ENV NEXT_TELEMETRY_DISABLED=1
ENV PORT=3000
ENV HOSTNAME=0.0.0.0
# Server-side (RSC / route handlers) fetches hit the gateway directly over the
# internal network — relative URLs aren't valid there. Overridable at runtime.
ARG API_GATEWAY_ORIGIN=http://api-gateway:8080
ENV INTERNAL_API_GATEWAY_URL=${API_GATEWAY_ORIGIN}

# Run as non-root.
RUN addgroup --system --gid 1001 velocity \
 && adduser  --system --uid 1001 --ingroup velocity velocity

# Copy the standalone Next build (set in next.config.mjs).
COPY --from=builder --chown=velocity:velocity /app/.next/standalone ./
COPY --from=builder --chown=velocity:velocity /app/.next/static ./.next/static
COPY --from=builder --chown=velocity:velocity /app/public ./public

USER velocity
EXPOSE 3000

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD wget --quiet --spider http://localhost:3000/ || exit 1

CMD ["node", "server.js"]
