"""Online Isolation Forest fitted on a sliding window of leaderboard
features.

Why Isolation Forest?
---------------------
We want a per-submission health badge with two properties:
  1. No labels — submissions have no ground-truth "anomalous" tag.
  2. Robust to multi-modal cluster shapes — submissions cluster
     naturally by language / strategy / persona mix, and a single-mode
     Gaussian baseline would flag every legitimate variant as
     anomalous.

Isolation Forest delivers both. A small ensemble (default 100 trees)
trained on a sliding window (last N submissions or last T minutes)
isolates a sample with a path length proportional to its anomaly
score. We don't need the score's absolute calibration — we threshold
at the 2nd percentile of the *current* window to get a stable
"WATCH/ANOMALY" cutoff regardless of cluster drift.

The model is held in-process and refit cheaply (~50ms for 1000
samples) on a 60s cadence; queries against the latest fit are O(log
n) so the FastAPI endpoint stays sub-ms.

Feature set
-----------
The 8 features below are deliberately spartan — adding more pushes
the model toward overfitting the small sample. The set is:

  composite_score, sustained_rps_log, p50_ns_log, p99_ns_log,
  p999_ns_log, correctness_ratio, error_ratio, cliff_detected

Logs let extreme tail values map smoothly without dominating the
forest's variance.
"""

from __future__ import annotations

import math
import threading
from collections import deque
from dataclasses import dataclass, field
from typing import Deque

import numpy as np
from sklearn.ensemble import IsolationForest


FEATURE_COUNT = 8


@dataclass
class SubmissionFeatures:
    submission_id:    str
    composite_score:  float
    sustained_rps:    float
    p50_ns:           float
    p99_ns:           float
    p999_ns:          float
    correctness:      float    # [0,100]
    error_ratio:      float    # [0,1]
    cliff_detected:   bool
    captured_at_ms:   int

    def to_vector(self) -> np.ndarray:
        return np.array([
            self.composite_score,
            math.log1p(max(self.sustained_rps, 0.0)),
            math.log1p(max(self.p50_ns,  0.0)),
            math.log1p(max(self.p99_ns,  0.0)),
            math.log1p(max(self.p999_ns, 0.0)),
            self.correctness / 100.0,
            self.error_ratio,
            1.0 if self.cliff_detected else 0.0,
        ], dtype=np.float32)


# Health labels surfaced to the leaderboard.
HEALTH_OK       = "ok"
HEALTH_WATCH    = "watch"
HEALTH_ANOMALY  = "anomaly"


@dataclass
class AnomalyResult:
    submission_id: str
    score:         float       # raw IsolationForest decision_function output
    rank_pct:      float       # percentile of THIS score within the window
    health:        str         # one of HEALTH_*
    reason:        str         # human-readable summary


@dataclass
class DetectorConfig:
    window_size:   int   = 256       # sliding window of submissions
    refit_every:   int   = 8         # samples between refits
    contamination: float = 0.05      # IF's prior on anomaly fraction
    seed:          int   = 0
    # Anomaly = score below the 2nd percentile; watch = bottom 10 %.
    # IsolationForest.decision_function: higher = more normal, lower = more anomalous.
    anomaly_pct: float = 2.0
    watch_pct:   float = 10.0


class IsolationForestDetector:
    """Thread-safe wrapper that maintains a fitted IsolationForest on a
    sliding window of submission feature vectors."""

    def __init__(self, cfg: DetectorConfig | None = None):
        self.cfg = cfg or DetectorConfig()
        self._lock = threading.Lock()
        self._buf: Deque[np.ndarray] = deque(maxlen=self.cfg.window_size)
        self._ids: Deque[str] = deque(maxlen=self.cfg.window_size)
        self._model: IsolationForest | None = None
        self._scores: np.ndarray | None = None
        self._since_last_fit = 0

    def observe(self, feats: SubmissionFeatures) -> AnomalyResult:
        """Add a feature vector to the window and return the (possibly
        re-fitted) anomaly verdict for it."""
        vec = feats.to_vector()
        with self._lock:
            # If we already have an entry for this submission, replace
            # it — we only want one row per submission so the model
            # doesn't over-weight long-running benchmarks.
            if feats.submission_id in self._ids:
                idx = list(self._ids).index(feats.submission_id)
                # Remove and re-append at the right (newest) end.
                self._ids.remove(feats.submission_id)
                # Deque doesn't support remove-by-index for the buffer;
                # rebuild from the list of vectors minus the old entry.
                vecs = list(self._buf)
                del vecs[idx]
                self._buf = deque(vecs, maxlen=self.cfg.window_size)
            self._ids.append(feats.submission_id)
            self._buf.append(vec)
            self._since_last_fit += 1

            need_fit = (
                self._model is None
                or self._since_last_fit >= self.cfg.refit_every
                # IF needs ≥ 2 samples to fit. We also avoid fitting on
                # a tiny window where every sample is "anomalous".
            )
            if need_fit and len(self._buf) >= 32:
                self._fit_locked()
                self._since_last_fit = 0

            return self._score_locked(feats)

    def snapshot(self) -> list[AnomalyResult]:
        """Re-evaluate the entire window. Useful for cold-start refills
        when downstream callers (e.g. the gateway badge endpoint) need
        the full state."""
        with self._lock:
            if self._model is None or not self._buf:
                return []
            return [
                self._score_for_vector_locked(sid, vec)
                for sid, vec in zip(self._ids, self._buf)
            ]

    # ---------- private ---------------------------------------------------

    def _fit_locked(self):
        X = np.array(list(self._buf), dtype=np.float32)
        self._model = IsolationForest(
            n_estimators=100,
            max_samples=min(256, len(X)),
            contamination=self.cfg.contamination,
            random_state=self.cfg.seed,
            n_jobs=1,
        )
        self._model.fit(X)
        self._scores = self._model.decision_function(X)

    def _score_locked(self, feats: SubmissionFeatures) -> AnomalyResult:
        if self._model is None:
            return AnomalyResult(
                submission_id=feats.submission_id,
                score=0.0, rank_pct=50.0,
                health=HEALTH_OK,
                reason="model warming up",
            )
        return self._score_for_vector_locked(feats.submission_id, feats.to_vector())

    def _score_for_vector_locked(self, sid: str, vec: np.ndarray) -> AnomalyResult:
        assert self._model is not None
        s = float(self._model.decision_function(vec.reshape(1, -1))[0])
        assert self._scores is not None
        pct = float(np.searchsorted(np.sort(self._scores), s) / len(self._scores) * 100)

        if pct <= self.cfg.anomaly_pct:
            health, reason = HEALTH_ANOMALY, "below 2nd-pct in current window"
        elif pct <= self.cfg.watch_pct:
            health, reason = HEALTH_WATCH, "below 10th-pct in current window"
        else:
            health, reason = HEALTH_OK, "within nominal cluster"

        return AnomalyResult(
            submission_id=sid,
            score=s,
            rank_pct=pct,
            health=health,
            reason=reason,
        )
