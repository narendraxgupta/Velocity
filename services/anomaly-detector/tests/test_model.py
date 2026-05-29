"""Smoke + behavioural tests for IsolationForestDetector.

Run with ``pytest`` from the service root (``services/anomaly-detector``).
The fixtures are deliberately small (≤ 100 samples) so the suite stays
under 1s in CI.
"""

import random

import numpy as np

from anomaly_detector.model import (
    DetectorConfig,
    HEALTH_ANOMALY,
    HEALTH_OK,
    HEALTH_WATCH,
    IsolationForestDetector,
    SubmissionFeatures,
)


def _normal_feats(seed: int) -> SubmissionFeatures:
    rng = random.Random(seed)
    return SubmissionFeatures(
        submission_id=f"sub-{seed}",
        composite_score=80 + rng.uniform(-3, 3),
        sustained_rps=15_000 + rng.uniform(-500, 500),
        p50_ns=80_000 + rng.uniform(-5_000, 5_000),
        p99_ns=250_000 + rng.uniform(-10_000, 10_000),
        p999_ns=600_000 + rng.uniform(-20_000, 20_000),
        correctness=99 + rng.uniform(-0.5, 0.5),
        error_ratio=0.001 + rng.uniform(0, 0.0005),
        cliff_detected=False,
        captured_at_ms=0,
    )


def _outlier_feats(seed: int) -> SubmissionFeatures:
    return SubmissionFeatures(
        submission_id=f"outlier-{seed}",
        composite_score=12,
        sustained_rps=200,
        p50_ns=8_000_000,
        p99_ns=80_000_000,
        p999_ns=200_000_000,
        correctness=20,
        error_ratio=0.7,
        cliff_detected=True,
        captured_at_ms=0,
    )


def test_detector_flags_outlier_after_warmup():
    det = IsolationForestDetector(DetectorConfig(window_size=128, refit_every=4, seed=1))
    for i in range(80):
        det.observe(_normal_feats(i))
    out = det.observe(_outlier_feats(0))
    assert out.health in {HEALTH_ANOMALY, HEALTH_WATCH}, (
        f"expected outlier to be anomaly/watch, got {out.health} (pct={out.rank_pct:.2f})")


def test_detector_marks_normal_ok():
    det = IsolationForestDetector(DetectorConfig(window_size=128, refit_every=4, seed=2))
    for i in range(80):
        det.observe(_normal_feats(i))
    ok = det.observe(_normal_feats(999))
    assert ok.health == HEALTH_OK, f"normal sample not OK: {ok}"


def test_cold_start_returns_warmup_label():
    det = IsolationForestDetector(DetectorConfig(window_size=128, refit_every=4))
    out = det.observe(_normal_feats(0))
    assert out.health == HEALTH_OK
    assert "warming" in out.reason


def test_snapshot_returns_all_rows():
    det = IsolationForestDetector(DetectorConfig(window_size=64, refit_every=2, seed=3))
    for i in range(40):
        det.observe(_normal_feats(i))
    snap = det.snapshot()
    assert len(snap) == 40
    assert all(r.health in {HEALTH_OK, HEALTH_WATCH, HEALTH_ANOMALY} for r in snap)


def test_duplicate_submission_replaces_in_window():
    det = IsolationForestDetector(DetectorConfig(window_size=64, refit_every=2, seed=4))
    for i in range(20):
        det.observe(_normal_feats(i))
    # Same submission_id observed twice — should not grow the window
    # beyond N+1 (the duplicate replaces the old row).
    det.observe(_normal_feats(5))
    snap = det.snapshot()
    ids = [r.submission_id for r in snap]
    assert len(ids) == len(set(ids)), f"duplicate ids in snapshot: {ids}"
