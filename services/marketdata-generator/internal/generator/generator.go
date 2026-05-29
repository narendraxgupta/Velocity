// Package generator implements a stochastic reference price generator.
//
// Model
// -----
// We drive the mid price of each configured symbol with an
// Ornstein-Uhlenbeck (OU) process — mean-reverting, normally
// distributed innovations, exactly the workhorse model microstructure
// papers use as a "neutral" baseline:
//
//	dX_t = θ (μ − X_t) dt + σ dW_t
//
// Discretized with the closed-form solution (since the OU SDE is one of
// the few that has one and we lose nothing by using it):
//
//	X_{t+Δt} = X_t e^{-θΔt} + μ (1 − e^{-θΔt})
//	         + σ √((1 − e^{-2θΔt}) / (2θ)) · Z   where Z ~ N(0,1)
//
// On top of that we layer a compound Poisson "shock" process:
//
//	jumps ~ Poisson(λ Δt)
//	shock size = J · sign  where J ~ N(μ_J, σ_J), sign ~ ±1
//
// Shocks model news-event-driven discontinuities. Real exchanges see a
// dozen of these a day on liquid symbols; the default λ = 1/300s (~12
// per hour) puts us comfortably in that range while staying interesting
// to look at over a 60-second benchmark window.
//
// Correlated multi-symbol fan-out
// -------------------------------
// Each symbol has its own (θ, μ, σ, λ). When multiple symbols are
// configured, their Z draws come from the SAME Wiener vector
// pre-multiplied by a Cholesky factor, so SPOT/PERP/FUTURES are
// correlated as you'd expect from real venues. The default config wires
// SPOT-PERP ρ=0.95, SPOT-FUTURES ρ=0.85, PERP-FUTURES ρ=0.88.
//
// Outputs
// -------
// Every tick is fan-outed to:
//   - Redis: `marketdata:<symbol>:mid_units` (fixed-point) with a 60s TTL
//   - Redpanda (optional): topic `marketdata.ticks`, one message per
//     symbol per tick — useful when downstream consumers want history
//
// The default cadence is 100 Hz (Δt = 10ms) which matches the bot
// worker's reactor wake-up cadence — fair_value reads stay coherent.
package generator

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"math/rand"
	"strconv"
	"sync"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/twmb/franz-go/pkg/kgo"
	"go.uber.org/zap"
)

// SymbolConfig parameterises one OU + jump path.
type SymbolConfig struct {
	Symbol       string  // wire-level symbol, e.g. "SPOT/USDT"
	InitialPrice float64 // starting mid (display units)
	Theta        float64 // mean-reversion speed (1/s)
	Mu           float64 // long-run mean (display units)
	Sigma        float64 // instantaneous vol (display units / √s)
	JumpRate     float64 // expected jumps per second (Poisson λ)
	JumpMean     float64 // E[|jump|], display units
	JumpStddev   float64 // jump magnitude vol, display units
	PriceScale   int64   // fixed-point scale to publish (e.g. 1_000_000 ⇒ 6 decimals)
}

// Config wires the generator.
type Config struct {
	TickInterval time.Duration  // ≈10ms in production
	Symbols      []SymbolConfig // ordered; correlation matrix is positional
	Correlations [][]float64    // square, len(Symbols)×len(Symbols); identity if nil
	Redis        *redis.Client
	Kafka        *kgo.Client // nil → don't publish to Redpanda
	KafkaTopic   string
	Logger       *zap.SugaredLogger
	Seed         int64 // deterministic seeding for reproducible benchmarks
}

// Generator is the long-lived worker.
type Generator struct {
	cfg Config

	mu      sync.Mutex
	prices  []float64
	chol    [][]float64 // lower triangular Cholesky factor of Correlations
	rng     *rand.Rand
	stopped bool
}

// New constructs a Generator. Returns an error if the correlation
// matrix is not positive semi-definite.
func New(cfg Config) (*Generator, error) {
	if cfg.TickInterval <= 0 {
		cfg.TickInterval = 10 * time.Millisecond
	}
	if len(cfg.Symbols) == 0 {
		return nil, errors.New("at least one symbol required")
	}
	n := len(cfg.Symbols)
	if cfg.Correlations == nil {
		cfg.Correlations = identity(n)
	}
	if len(cfg.Correlations) != n {
		return nil, fmt.Errorf("correlation matrix is %dx? but expected %dx%d",
			len(cfg.Correlations), n, n)
	}
	chol, err := cholesky(cfg.Correlations)
	if err != nil {
		return nil, fmt.Errorf("cholesky: %w", err)
	}
	prices := make([]float64, n)
	for i, s := range cfg.Symbols {
		prices[i] = s.InitialPrice
		if s.PriceScale == 0 {
			cfg.Symbols[i].PriceScale = 1_000_000
		}
	}
	seed := cfg.Seed
	if seed == 0 {
		seed = time.Now().UnixNano()
	}
	return &Generator{
		cfg:    cfg,
		prices: prices,
		chol:   chol,
		rng:    rand.New(rand.NewSource(seed)),
	}, nil
}

// Run drives the generator until ctx is cancelled. Tick errors are
// logged but never bubble out — a single Redis hiccup must not stop the
// price feed.
func (g *Generator) Run(ctx context.Context) error {
	g.cfg.Logger.Infow("marketdata generator starting",
		"symbols", len(g.cfg.Symbols),
		"tick_interval", g.cfg.TickInterval)

	t := time.NewTicker(g.cfg.TickInterval)
	defer t.Stop()

	for {
		select {
		case <-ctx.Done():
			g.cfg.Logger.Info("marketdata generator shutting down")
			return nil
		case now := <-t.C:
			g.advance(g.cfg.TickInterval.Seconds())
			g.publish(ctx, now)
		}
	}
}

// Snapshot returns the current price of every configured symbol.
// Useful for debugging and HTTP introspection.
func (g *Generator) Snapshot() map[string]float64 {
	g.mu.Lock()
	defer g.mu.Unlock()
	out := make(map[string]float64, len(g.prices))
	for i, s := range g.cfg.Symbols {
		out[s.Symbol] = g.prices[i]
	}
	return out
}

// -----------------------------------------------------------------------------
//  Internal: OU step + jump
// -----------------------------------------------------------------------------

func (g *Generator) advance(dt float64) {
	g.mu.Lock()
	defer g.mu.Unlock()

	// Draw an n-dimensional correlated standard-normal vector.
	n := len(g.prices)
	z := make([]float64, n)
	for i := range z {
		z[i] = g.rng.NormFloat64()
	}
	// y = chol · z  (multivariate normal with covariance = Correlations)
	y := make([]float64, n)
	for i := 0; i < n; i++ {
		var s float64
		for k := 0; k <= i; k++ {
			s += g.chol[i][k] * z[k]
		}
		y[i] = s
	}

	for i, sym := range g.cfg.Symbols {
		// Closed-form OU step.
		expDecay := math.Exp(-sym.Theta * dt)
		// Per-step stdev. As Theta → 0 the OU process degenerates to
		// Brownian motion and the variance term (1-e^{-2θdt})/(2θ) → dt.
		// Evaluating the closed form at Theta == 0 divides 0/0 = NaN,
		// which then poisons every price for the rest of the run, so take
		// the Brownian limit explicitly. (Theta < 0 is nonsensical for a
		// mean-reversion speed; fall back to the same safe limit.)
		var stdev float64
		if sym.Theta > 0 {
			stdev = sym.Sigma * math.Sqrt((1-math.Exp(-2*sym.Theta*dt))/(2*sym.Theta))
		} else {
			stdev = sym.Sigma * math.Sqrt(dt)
		}
		next := g.prices[i]*expDecay + sym.Mu*(1-expDecay) + stdev*y[i]

		// Compound-Poisson jump component. With small dt and modest λ
		// we can approximate Poisson(λ dt) by Bernoulli(λ dt) — at the
		// 100 Hz cadence this is exact up to second order in λ dt.
		if g.rng.Float64() < sym.JumpRate*dt {
			sign := 1.0
			if g.rng.Float64() < 0.5 {
				sign = -1.0
			}
			mag := sym.JumpMean + sym.JumpStddev*g.rng.NormFloat64()
			if mag < 0 {
				mag = -mag
			}
			next += sign * mag
		}
		// Floor at the tick (a tradable price can't be ≤ 0).
		if next < 1.0/float64(sym.PriceScale) {
			next = 1.0 / float64(sym.PriceScale)
		}
		g.prices[i] = next
	}
}

func (g *Generator) publish(ctx context.Context, now time.Time) {
	g.mu.Lock()
	prices := make([]float64, len(g.prices))
	copy(prices, g.prices)
	g.mu.Unlock()

	for i, sym := range g.cfg.Symbols {
		fixed := int64(prices[i] * float64(sym.PriceScale))
		if g.cfg.Redis != nil {
			key := "marketdata:" + sym.Symbol + ":mid_units"
			if err := g.cfg.Redis.Set(ctx, key, strconv.FormatInt(fixed, 10), 60*time.Second).Err(); err != nil {
				g.cfg.Logger.Warnw("redis set failed", "key", key, "err", err)
			}
		}
		if g.cfg.Kafka != nil && g.cfg.KafkaTopic != "" {
			body, _ := json.Marshal(map[string]any{
				"symbol":     sym.Symbol,
				"mid_units":  fixed,
				"mid":        prices[i],
				"price_scale": sym.PriceScale,
				"ts_ns":      now.UnixNano(),
			})
			rec := &kgo.Record{
				Topic: g.cfg.KafkaTopic,
				Key:   []byte(sym.Symbol),
				Value: body,
			}
			g.cfg.Kafka.Produce(ctx, rec, func(_ *kgo.Record, err error) {
				if err != nil {
					g.cfg.Logger.Warnw("kafka produce failed",
						"symbol", sym.Symbol, "err", err)
				}
			})
		}
	}
}

// -----------------------------------------------------------------------------
//  Linear algebra helpers
// -----------------------------------------------------------------------------

func identity(n int) [][]float64 {
	out := make([][]float64, n)
	for i := range out {
		out[i] = make([]float64, n)
		out[i][i] = 1.0
	}
	return out
}

// cholesky returns the lower triangular L such that L Lᵀ = A. Assumes A
// is symmetric and positive definite; returns an error otherwise.
func cholesky(a [][]float64) ([][]float64, error) {
	n := len(a)
	l := make([][]float64, n)
	for i := range l {
		l[i] = make([]float64, n)
	}
	for i := 0; i < n; i++ {
		for j := 0; j <= i; j++ {
			var s float64
			for k := 0; k < j; k++ {
				s += l[i][k] * l[j][k]
			}
			if i == j {
				v := a[i][i] - s
				if v <= 0 {
					return nil, fmt.Errorf("matrix not positive-definite at row %d", i)
				}
				l[i][j] = math.Sqrt(v)
			} else {
				if l[j][j] == 0 {
					return nil, fmt.Errorf("zero pivot at row %d", j)
				}
				l[i][j] = (a[i][j] - s) / l[j][j]
			}
		}
	}
	return l, nil
}
