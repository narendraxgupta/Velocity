"""Latency regression detector.

Goal
----
Given two HdrHistogram-style bucket arrays — the most recent run for a
submission and a chosen baseline (typically the previous run on the
same submission ID) — decide whether the latency distribution has
*statistically significantly* worsened. Output:

  - ks_stat:     KS statistic D
  - ks_pvalue:   two-sample two-sided KS p-value
  - p50/p99/p999 deltas, in nanoseconds
  - regression:  "improved" | "stable" | "regressed"
  - human_summary: 1-line readable verdict, e.g.
        "+42µs at p99 (KS p=0.003, n=4096) — REGRESSION"

We use the *two-sample* KS test on the *quantile distributions*
implied by the buckets (i.e. expand each bucket to a frequency-weighted
sample, then compare). At realistic bucket counts (a few hundred) this
is fast and well-calibrated.

Storage
-------
Persisted histograms live at:

    histogram:<submission_id>:latest    JSON {buckets, edges_ns, ts_ms}

The detector writes a new "latest" on every benchmark complete and
keeps the OLD value at:

    histogram:<submission_id>:baseline  (same shape, updated to N-1)

That two-tier layout means "current vs baseline" is always available
without needing a cross-run scan.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, asdict
from typing import Iterable

import numpy as np
from scipy import stats


# --- Bucket / Histogram envelope --------------------------------------------

@dataclass
class HistogramEnvelope:
    """A frozen HdrHistogram snapshot — just enough to compare two of them."""
    buckets: list[int]      # bucket counts, length N
    edges_ns: list[int]     # bucket UPPER edges, length N. edges_ns[i] = value < edge
    ts_ms: int              # capture time
    submission_id: str
    benchmark_id: str

    def total_count(self) -> int:
        return sum(self.buckets)

    def percentile(self, p: float) -> float:
        """Linear-interpolated percentile within the bucket the threshold
        lands in. p ∈ [0,100]."""
        n = self.total_count()
        if n == 0:
            return 0.0
        target = p / 100.0 * n
        cum = 0
        for i, count in enumerate(self.buckets):
            if cum + count >= target:
                lo_edge = self.edges_ns[i - 1] if i > 0 else 0
                hi_edge = self.edges_ns[i]
                # Fraction within the bucket
                frac = (target - cum) / max(count, 1)
                return lo_edge + frac * (hi_edge - lo_edge)
            cum += count
        return float(self.edges_ns[-1])

    def expand_samples(self, max_samples: int = 4096) -> np.ndarray:
        """Expand bucket counts into approximate samples (capped to
        max_samples to keep KS comparable across very different
        sample sizes). We place each sample at the bucket midpoint."""
        n = self.total_count()
        if n == 0:
            return np.zeros(0, dtype=np.float64)
        cap = min(n, max_samples)
        out = np.zeros(cap, dtype=np.float64)
        idx = 0
        for i, count in enumerate(self.buckets):
            if count == 0:
                continue
            lo = self.edges_ns[i - 1] if i > 0 else 0
            hi = self.edges_ns[i]
            mid = (lo + hi) / 2.0
            take = max(1, round(count / n * cap))
            take = min(take, cap - idx)
            if take <= 0:
                break
            out[idx:idx + take] = mid
            idx += take
            if idx >= cap:
                break
        return out[:idx]


@dataclass
class RegressionResult:
    submission_id:    str
    baseline_ts_ms:   int
    current_ts_ms:    int
    n_baseline:       int
    n_current:        int
    ks_stat:          float
    ks_pvalue:        float
    p50_delta_ns:     float
    p99_delta_ns:     float
    p999_delta_ns:    float
    regression:       str        # "improved" | "stable" | "regressed"
    human_summary:    str


# --- KS-driven verdict ------------------------------------------------------

# Significance threshold. We use p < 0.01 plus a meaningful p99 delta
# (≥ 5µs) to avoid statistically-significant-but-clinically-trivial
# noise from sample-size differences.
_KS_PVALUE_REJECT  = 0.01
_P99_DELTA_NS_MIN  = 5_000          # 5 µs


def compare(baseline: HistogramEnvelope,
            current: HistogramEnvelope) -> RegressionResult:
    """Run a KS test and produce a verdict."""
    if baseline.total_count() == 0 or current.total_count() == 0:
        return _zero_result(baseline, current,
                            "insufficient data in baseline or current")

    a = baseline.expand_samples()
    b = current.expand_samples()
    if a.size == 0 or b.size == 0:
        return _zero_result(baseline, current,
                            "insufficient data in baseline or current")

    ks = stats.ks_2samp(a, b, alternative="two-sided", method="auto")
    ks_stat   = float(ks.statistic)
    ks_pvalue = float(ks.pvalue)

    p50_d  = current.percentile(50)  - baseline.percentile(50)
    p99_d  = current.percentile(99)  - baseline.percentile(99)
    p999_d = current.percentile(99.9) - baseline.percentile(99.9)

    significant = ks_pvalue < _KS_PVALUE_REJECT
    if significant and p99_d > _P99_DELTA_NS_MIN:
        verdict = "regressed"
    elif significant and p99_d < -_P99_DELTA_NS_MIN:
        verdict = "improved"
    else:
        verdict = "stable"

    summary = _format_summary(verdict, p99_d, ks_pvalue,
                              current.total_count(), baseline.total_count())

    return RegressionResult(
        submission_id   = current.submission_id,
        baseline_ts_ms  = baseline.ts_ms,
        current_ts_ms   = current.ts_ms,
        n_baseline      = baseline.total_count(),
        n_current       = current.total_count(),
        ks_stat         = ks_stat,
        ks_pvalue       = ks_pvalue,
        p50_delta_ns    = p50_d,
        p99_delta_ns    = p99_d,
        p999_delta_ns   = p999_d,
        regression      = verdict,
        human_summary   = summary,
    )


def _zero_result(baseline: HistogramEnvelope, current: HistogramEnvelope,
                 note: str) -> RegressionResult:
    return RegressionResult(
        submission_id   = current.submission_id,
        baseline_ts_ms  = baseline.ts_ms,
        current_ts_ms   = current.ts_ms,
        n_baseline      = baseline.total_count(),
        n_current       = current.total_count(),
        ks_stat         = 0.0,
        ks_pvalue       = 1.0,
        p50_delta_ns    = 0.0,
        p99_delta_ns    = 0.0,
        p999_delta_ns   = 0.0,
        regression      = "stable",
        human_summary   = note,
    )


def _format_summary(verdict: str, p99_delta_ns: float, p: float,
                    n_curr: int, n_base: int) -> str:
    sign = "+" if p99_delta_ns >= 0 else ""
    dispatch = {
        "regressed": "REGRESSION",
        "improved":  "IMPROVEMENT",
        "stable":    "stable",
    }[verdict]
    return (f"{sign}{p99_delta_ns / 1000:.1f}µs at p99 "
            f"(KS p={p:.3g}, n_base={n_base}, n_curr={n_curr}) — {dispatch}")


# --- JSON envelope helpers (used by the FastAPI route) ----------------------

def envelope_from_json(body: dict) -> HistogramEnvelope:
    """Build a HistogramEnvelope from the gateway's POST payload.
    Validates lengths; throws ValueError on inconsistency."""
    buckets  = body.get("buckets", [])
    edges_ns = body.get("edges_ns", [])
    if not isinstance(buckets, list) or not isinstance(edges_ns, list):
        raise ValueError("buckets and edges_ns must be lists")
    if len(buckets) != len(edges_ns):
        raise ValueError("buckets/edges_ns length mismatch")
    return HistogramEnvelope(
        buckets       = [int(x) for x in buckets],
        edges_ns      = [int(x) for x in edges_ns],
        ts_ms         = int(body.get("ts_ms", 0)),
        submission_id = str(body.get("submission_id", "")),
        benchmark_id  = str(body.get("benchmark_id", "")),
    )


def serialize_result(r: RegressionResult) -> dict:
    return asdict(r)
