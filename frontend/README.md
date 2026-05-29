# Velocity Frontend

Next.js 14 + TailwindCSS + Bloomberg × Linear hybrid theme.

## Design system

The theme lives in two places:

1. **[`app/globals.css`](app/globals.css)** — the design tokens (HSL triplets
   stored as CSS custom properties). Re-skinning the entire app is one edit
   in `:root`.
2. **[`tailwind.config.ts`](tailwind.config.ts)** — the Tailwind mapping that
   exposes the tokens as utility classes (`bg-surface`, `text-signal-live`,
   `shadow-glow-accent`, …).

## Component primitives

- **`<Panel>`** — the universal dashboard container (`components/ui/panel.tsx`).
- **`<MetricCard>`** — a labeled number tile (`components/metric-card.tsx`).
- **`<TopNav>` + `<TickerStrip>`** — the persistent header (`components/top-nav.tsx`).
- **`<LeaderboardPreview>`** — the dense, terminal-style ranked table.

## Local dev

```bash
npm install
npm run dev
# open http://localhost:3000
```

## Production build

```bash
npm run build
npm run start
```

…or, more usefully, via the platform's Docker Compose:

```bash
cd ..
make up   # builds and starts every service including this one
```

## File layout

```
frontend/
├── app/                       Next 14 App Router
│   ├── layout.tsx             Root shell (fonts + chrome)
│   ├── page.tsx               Landing + dashboard
│   ├── leaderboard/page.tsx   Live ranked view
│   ├── submissions/page.tsx   Upload + lifecycle (Phase 2)
│   └── globals.css            Theme tokens + base styles
├── components/
│   ├── top-nav.tsx
│   ├── ticker-strip.tsx
│   ├── hero.tsx
│   ├── metric-card.tsx
│   ├── ui/
│   │   └── panel.tsx
│   └── leaderboard/
│       └── leaderboard-preview.tsx
├── lib/
│   └── utils.ts               cn() + number formatters
├── styles/                    Reserved for component-scoped CSS modules
└── public/                    Static assets
```
