package generator

import (
	"context"
	"math"
	"testing"
	"time"

	"go.uber.org/zap"
)

// Deterministic numerical regression: a single-symbol OU with σ=0 must
// collapse to its closed-form mean reverter, modulo rounding from the
// publish/fixed-point hop.
func TestAdvance_ZeroVolReturnsClosedFormOU(t *testing.T) {
	cfg := Config{
		TickInterval: 10 * time.Millisecond,
		Symbols: []SymbolConfig{{
			Symbol: "X", InitialPrice: 50, Theta: 1.0, Mu: 100, Sigma: 0,
			JumpRate: 0, PriceScale: 1_000_000,
		}},
		Logger: zap.NewNop().Sugar(),
		Seed:   42,
	}
	g, err := New(cfg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	// 1 second of evolution.
	dt := 0.01
	for i := 0; i < 100; i++ {
		g.advance(dt)
	}
	// Closed form: X(t) = X0 e^{-θt} + μ(1-e^{-θt}). t = 1, θ = 1.
	expected := 50*math.Exp(-1) + 100*(1-math.Exp(-1))
	got := g.Snapshot()["X"]
	if math.Abs(got-expected) > 1e-9 {
		t.Fatalf("OU mean-reversion drift: want %.9f got %.9f", expected, got)
	}
}

// With strong positive correlation, two symbols sharing μ should track
// each other closely. Sanity check (not a unit test on numerics — those
// live in the Cholesky path).
func TestAdvance_CorrelatedSymbols(t *testing.T) {
	cfg := Config{
		TickInterval: 10 * time.Millisecond,
		Symbols: []SymbolConfig{
			{Symbol: "A", InitialPrice: 100, Theta: 0.1, Mu: 100, Sigma: 0.5, PriceScale: 1_000_000},
			{Symbol: "B", InitialPrice: 100, Theta: 0.1, Mu: 100, Sigma: 0.5, PriceScale: 1_000_000},
		},
		Correlations: [][]float64{{1.0, 0.99}, {0.99, 1.0}},
		Logger:       zap.NewNop().Sugar(),
		Seed:         1,
	}
	g, err := New(cfg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	for i := 0; i < 1000; i++ {
		g.advance(0.01)
	}
	snap := g.Snapshot()
	// With ρ=0.99 the paths should be within a few sigmas of each other
	// — over a 10s window with σ=0.5/√s and 99% correlation the
	// path-difference std is √(2(1-ρ)) σ √t ≈ 0.22, so 5 sigmas is 1.1.
	if math.Abs(snap["A"]-snap["B"]) > 2.0 {
		t.Fatalf("highly-correlated paths drifted too far: A=%.4f B=%.4f", snap["A"], snap["B"])
	}
}

func TestCholesky_RejectsNonPositiveDefinite(t *testing.T) {
	// Off-diagonal > 1: not a valid correlation matrix.
	_, err := cholesky([][]float64{{1.0, 1.5}, {1.5, 1.0}})
	if err == nil {
		t.Fatal("expected positive-definiteness error")
	}
}

func TestCholesky_KnownDecomposition(t *testing.T) {
	// 2x2 with ρ=0.5: L = [[1,0],[0.5,sqrt(0.75)]]
	l, err := cholesky([][]float64{{1.0, 0.5}, {0.5, 1.0}})
	if err != nil {
		t.Fatal(err)
	}
	if math.Abs(l[0][0]-1.0) > 1e-12 ||
		math.Abs(l[1][0]-0.5) > 1e-12 ||
		math.Abs(l[1][1]-math.Sqrt(0.75)) > 1e-12 {
		t.Fatalf("unexpected L: %v", l)
	}
}

// Smoke-test: Run shuts down cleanly on ctx cancel without panic, even
// with no Redis or Kafka clients wired.
func TestRun_GracefulShutdown(t *testing.T) {
	g, err := New(Config{
		TickInterval: 5 * time.Millisecond,
		Symbols:      []SymbolConfig{{Symbol: "S", InitialPrice: 10, Mu: 10, Theta: 1, Sigma: 0.1, PriceScale: 1000}},
		Logger:       zap.NewNop().Sugar(),
		Seed:         7,
	})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Millisecond)
	defer cancel()
	if err := g.Run(ctx); err != nil {
		t.Fatalf("Run returned %v on cancel", err)
	}
}
