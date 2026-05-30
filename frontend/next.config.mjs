/**
 * Velocity — Next.js configuration
 *
 * `output: 'standalone'` produces a slim runtime bundle that the Dockerfile
 * copies into a distroless image. No external dependencies at runtime.
 */

/** @type {import('next').NextConfig} */
const nextConfig = {
  output: 'standalone',
  reactStrictMode: true,
  poweredByHeader: false,
  productionBrowserSourceMaps: false,
  experimental: {
    optimizePackageImports: ['lucide-react', 'echarts', 'echarts-for-react'],
  },
  // Same-origin API proxy. The browser calls `/v1/*` on its own origin (the
  // Next server) and we forward to the gateway over the internal network. This
  // is what lets the SPA work behind the GitHub Codespaces tunnel without
  // cross-origin CORS preflights (which the tunnel 403s) and without making
  // the gateway port public. The destination is resolved at BUILD time, so
  // API_GATEWAY_ORIGIN must be present as a build arg (defaults to the
  // docker-compose service name).
  async rewrites() {
    const gateway = process.env.API_GATEWAY_ORIGIN || 'http://api-gateway:8080'
    return [
      { source: '/v1/:path*', destination: `${gateway}/v1/:path*` },
    ]
  },
  async headers() {
    return [
      {
        source: '/(.*)',
        headers: [
          { key: 'X-Frame-Options', value: 'DENY' },
          { key: 'X-Content-Type-Options', value: 'nosniff' },
          { key: 'Referrer-Policy', value: 'strict-origin-when-cross-origin' },
          { key: 'Permissions-Policy', value: 'camera=(), microphone=(), geolocation=()' },
        ],
      },
    ]
  },
}

export default nextConfig
