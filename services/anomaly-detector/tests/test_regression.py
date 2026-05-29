"""Unit tests for the latency regression detector."""

from anomaly_detector.regression import (
    HistogramEnvelope,
    compare,
)


def make_envelope(counts: list[int], edges_ns: list[int], sid: str = "S",
                  bid: str = "B", ts_ms: int = 0) -> HistogramEnvelope:
    return HistogramEnvelope(
        buckets=counts, edges_ns=edges_ns, ts_ms=ts_ms,
        submission_id=sid, benchmark_id=bid,
    )


def test_compare_detects_regression_when_tail_shifts():
    # Baseline: 1000 samples concentrated under 100µs.
    edges = [10_000, 25_000, 50_000, 100_000, 250_000, 500_000, 1_000_000]
    base = make_envelope([100, 300, 400, 200, 0, 0, 0], edges)
    # Current: same head but a heavier 100-250µs tail.
    curr = make_envelope([50, 200, 300, 150, 250, 50, 0], edges)
    r = compare(base, curr)
    assert r.regression == "regressed", r
    assert r.p99_delta_ns > 0
    assert r.ks_pvalue < 0.01


def test_compare_marks_stable_when_identical():
    edges = [10_000, 25_000, 50_000, 100_000]
    base = make_envelope([100, 200, 200, 100], edges)
    curr = make_envelope([100, 200, 200, 100], edges)
    r = compare(base, curr)
    assert r.regression == "stable"
    assert r.p99_delta_ns == 0


def test_compare_marks_improved_when_tail_lighter():
    edges = [10_000, 25_000, 50_000, 100_000, 250_000]
    base = make_envelope([50, 150, 200, 100, 100], edges)
    curr = make_envelope([100, 250, 200, 50, 0], edges)
    r = compare(base, curr)
    # Could be "improved" or "stable" depending on KS significance — we
    # just assert it's not a false regression and the p99 has moved
    # left.
    assert r.regression != "regressed", r
    assert r.p99_delta_ns < 0


def test_compare_handles_empty_baseline():
    edges = [10_000, 25_000, 50_000]
    base = make_envelope([0, 0, 0], edges)
    curr = make_envelope([100, 200, 200], edges)
    r = compare(base, curr)
    assert r.regression == "stable"
    assert "insufficient" in r.human_summary
