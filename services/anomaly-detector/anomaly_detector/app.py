"""anomaly-detector FastAPI service.

This is a thin operational shell around the IsolationForestDetector:

    POST /v1/observe   → adds a submission's features, returns a verdict
                         and (idempotently) publishes the verdict to Redis.
    GET  /v1/health/{submission_id}   → latest cached verdict.
    GET  /v1/health                   → all cached verdicts (snapshot view).
    GET  /healthz, /readyz            → standard liveness / readiness.

Wire path
---------
The bot-controller writes per-tick leaderboard rows to Redis under
``leaderboard:row:<submission_id>``; we subscribe to the matching
keyspace events and observe each row. We also expose POST /v1/observe
so the gateway can feed historical data on cold-start.

Verdicts are published to ``health:<submission_id>`` with TTL 5
minutes so the leaderboard frontend can read them without going
through the gateway.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from dataclasses import asdict

import redis.asyncio as redis_async
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from .model import (
    DetectorConfig,
    HEALTH_ANOMALY,
    HEALTH_OK,
    HEALTH_WATCH,
    IsolationForestDetector,
    SubmissionFeatures,
)
from .regression import (
    HistogramEnvelope,
    compare as regression_compare,
    envelope_from_json,
    serialize_result,
)
from .adaptive import pick_profile


log = logging.getLogger("anomaly-detector")
logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"))


class ObservePayload(BaseModel):
    submission_id:    str
    composite_score:  float
    sustained_rps:    float
    p50_ns:           float
    p99_ns:           float
    p999_ns:          float
    correctness:      float
    error_ratio:      float
    cliff_detected:   bool = False
    captured_at_ms:   int  = Field(default_factory=lambda: int(time.time() * 1000))


def _to_features(p: ObservePayload) -> SubmissionFeatures:
    return SubmissionFeatures(**p.model_dump())


def make_app() -> FastAPI:
    cfg = DetectorConfig(
        window_size=int(os.getenv("ANOMALY_WINDOW_SIZE",   "256")),
        refit_every=int(os.getenv("ANOMALY_REFIT_EVERY",    "8")),
        contamination=float(os.getenv("ANOMALY_CONTAM",     "0.05")),
        seed=int(os.getenv("ANOMALY_SEED",                  "0")),
    )
    detector = IsolationForestDetector(cfg)
    state = {"redis": None, "subscriber_task": None}

    app = FastAPI(title="velocity-anomaly-detector", version="1.0.0")

    @app.on_event("startup")
    async def _startup():
        addr = os.getenv("ANOMALY_REDIS_ADDR", "redis://redis:6379/0")
        state["redis"] = redis_async.from_url(addr, decode_responses=True)
        try:
            await state["redis"].ping()
            log.info("redis connected: %s", addr)
        except Exception as exc:
            log.warning("redis ping failed (continuing): %s", exc)
        state["subscriber_task"] = asyncio.create_task(
            _leaderboard_subscriber(state["redis"], detector))

    @app.on_event("shutdown")
    async def _shutdown():
        task = state.get("subscriber_task")
        if task:
            task.cancel()
        r = state.get("redis")
        if r:
            await r.close()

    @app.post("/v1/observe")
    async def observe(payload: ObservePayload):
        feats = _to_features(payload)
        result = detector.observe(feats)
        await _publish(state["redis"], result)
        return asdict(result)

    @app.get("/v1/health/{submission_id}")
    async def get_health(submission_id: str):
        r = state.get("redis")
        if r is None:
            raise HTTPException(503, "redis not connected")
        raw = await r.get(f"health:{submission_id}")
        if raw is None:
            raise HTTPException(404, "no verdict cached")
        return json.loads(raw)

    @app.get("/v1/health")
    async def list_health():
        return [asdict(r) for r in detector.snapshot()]

    # ------------------------------------------------------------------
    #  Regression detector — KS-test on HdrHistogram buckets.
    # ------------------------------------------------------------------

    @app.post("/v1/regression/{submission_id}")
    async def upsert_histogram(submission_id: str, body: dict):
        """Stores a fresh histogram and (if a baseline exists) returns
        the KS-test verdict against it.

        The body is the post-flush HdrHistogram bucket array. We rotate
        the previous "latest" into "baseline" before persisting the
        new latest — that's how each upload gets compared to the run
        immediately preceding it.
        """
        r = state.get("redis")
        if r is None:
            raise HTTPException(503, "redis not connected")
        try:
            envelope = envelope_from_json(body)
        except ValueError as exc:
            raise HTTPException(400, f"invalid histogram envelope: {exc}")
        envelope.submission_id = submission_id

        # Rotate latest → baseline so we always compare current to N-1.
        try:
            existing = await r.get(f"histogram:{submission_id}:latest")
            if existing:
                await r.set(f"histogram:{submission_id}:baseline", existing,
                            ex=86_400 * 30)
        except Exception as exc:
            log.warning("redis rotate failed: %s", exc)

        # Persist the new "latest" for 30 days. We use a long TTL so
        # rare submitters can still run regression checks against runs
        # from weeks ago.
        envelope_json = json.dumps({
            "buckets":       envelope.buckets,
            "edges_ns":      envelope.edges_ns,
            "ts_ms":         envelope.ts_ms,
            "submission_id": envelope.submission_id,
            "benchmark_id":  envelope.benchmark_id,
        })
        try:
            await r.set(f"histogram:{submission_id}:latest", envelope_json,
                        ex=86_400 * 30)
        except Exception as exc:
            raise HTTPException(502, f"redis persist failed: {exc}")

        # If no baseline yet, return a neutral "stable" verdict.
        baseline_raw = await r.get(f"histogram:{submission_id}:baseline")
        if not baseline_raw:
            return {
                "regression": "stable",
                "human_summary": "first run on record — no baseline to compare to",
                "n_baseline": 0,
                "n_current":  envelope.total_count(),
            }
        baseline = envelope_from_json(json.loads(baseline_raw))
        result = regression_compare(baseline, envelope)
        await r.set(f"regression:{submission_id}",
                    json.dumps(serialize_result(result)),
                    ex=86_400 * 30)
        return serialize_result(result)

    @app.get("/v1/regression/{submission_id}")
    async def get_regression(submission_id: str):
        r = state.get("redis")
        if r is None:
            raise HTTPException(503, "redis not connected")
        raw = await r.get(f"regression:{submission_id}")
        if raw is None:
            raise HTTPException(404, "no regression data cached")
        return json.loads(raw)

    # ------------------------------------------------------------------
    #  Adaptive profile picker.
    # ------------------------------------------------------------------
    #
    # Reads everything we already cache about a submission (regression,
    # health, exec-quality, last benchmark report) and proposes the
    # benchmark profile name most likely to expose the *next* weakness.
    # The body is a POST with the last benchmark report (so callers can
    # pass through the gateway without giving the detector access to
    # the controller's gRPC surface) plus an optional override for the
    # exec_quality envelope.
    #
    # We expose this on POST for two reasons:
    #   1) the caller injects the freshest snapshot it has, not what
    #      Redis happens to have cached;
    #   2) the verb communicates "compute and tell me what's next" —
    #      this isn't a static GET resource.
    # ------------------------------------------------------------------

    @app.post("/v1/adaptive-profile/{submission_id}")
    async def adaptive_profile(submission_id: str, body: dict | None = None):
        r = state.get("redis")
        if r is None:
            raise HTTPException(503, "redis not connected")
        body = body or {}

        async def _fetch(key: str) -> dict | None:
            try:
                raw = await r.get(key)
                return json.loads(raw) if raw else None
            except Exception:
                return None

        regression_data = await _fetch(f"regression:{submission_id}")
        health_data     = await _fetch(f"health:{submission_id}")
        exec_quality    = await _fetch(f"exec_quality:{submission_id}")
        # Caller-supplied report wins over anything we have cached.
        last_report = body.get("report") or await _fetch(f"report:{submission_id}")

        pick = pick_profile(
            regression=regression_data,
            health=health_data,
            exec_quality=exec_quality,
            last_report=last_report,
        )
        result = {
            "submission_id": submission_id,
            "profile_name":  pick.profile_name,
            "reason":        pick.reason,
            "inputs":        pick.inputs,
        }
        try:
            await r.set(f"adaptive_profile:{submission_id}", json.dumps(result),
                        ex=86_400)
        except Exception as exc:
            log.warning("failed to cache adaptive profile pick: %s", exc)
        return result

    @app.get("/v1/adaptive-profile/{submission_id}")
    async def cached_adaptive_profile(submission_id: str):
        r = state.get("redis")
        if r is None:
            raise HTTPException(503, "redis not connected")
        raw = await r.get(f"adaptive_profile:{submission_id}")
        if not raw:
            raise HTTPException(404, "no adaptive profile cached")
        return json.loads(raw)

    @app.get("/healthz")
    async def healthz():
        return "ok"

    @app.get("/readyz")
    async def readyz():
        r = state.get("redis")
        if r is None:
            raise HTTPException(503, "no redis")
        try:
            await r.ping()
        except Exception as exc:
            raise HTTPException(503, str(exc))
        return "ready"

    return app


# ---------------------------------------------------------------------------
#  Redis subscriber
# ---------------------------------------------------------------------------

async def _publish(r, result):
    if r is None:
        return
    try:
        await r.set(
            f"health:{result.submission_id}",
            json.dumps(asdict(result)),
            ex=300,    # 5-minute TTL
        )
    except Exception as exc:
        log.warning("redis publish failed: %s", exc)


async def _leaderboard_subscriber(r, detector: IsolationForestDetector):
    """Listens on the leaderboard.delta pub/sub channel that the
    scoring-service publishes after every flush.

    Channel name is overridable via ANOMALY_LEADERBOARD_CHANNEL. The
    payload is the same JSON the leaderboard-ws bridge already
    consumes (see services/scoring-service/src/leaderboard_publisher.cpp),
    so we don't ask the scoring service to write a second key —
    duplication would be the worst kind of churn.

    For each upsert we observe the submission and persist the verdict
    to ``health:<submission_id>`` (TTL 5 min).
    """
    channel = os.getenv("ANOMALY_LEADERBOARD_CHANNEL", "leaderboard.global")
    if r is None:
        return
    try:
        ps = r.pubsub()
        await ps.subscribe(channel)
        log.info("subscribed to %s", channel)
        async for msg in ps.listen():
            if msg is None or msg.get("type") != "message":
                continue
            try:
                payload = json.loads(msg["data"])
                for row in payload.get("upserts", []):
                    sub_id = row.get("submission_id")
                    if not sub_id:
                        continue
                    # The published delta is a thin slice of SubmissionScore;
                    # we pull error_ratio / correctness off the same payload
                    # (correctness_score is on a 0-100 scale already).
                    feats = SubmissionFeatures(
                        submission_id   = sub_id,
                        composite_score = float(row.get("composite_score",  0.0)),
                        sustained_rps   = float(row.get("sustained_rps",    0.0)),
                        p50_ns          = float(row.get("p50_ns",           0.0)),
                        p99_ns          = float(row.get("p99_ns",           0.0)),
                        p999_ns         = float(row.get("p999_ns",          0.0)),
                        correctness     = float(row.get("correctness_score", 0.0)),
                        error_ratio     = float(row.get("error_ratio",      0.0)),
                        cliff_detected  = bool(row.get("cliff_detected", False)),
                        captured_at_ms  = int(row.get("updated_at_ns", 0) // 1_000_000)
                                          or int(time.time() * 1000),
                    )
                    result = detector.observe(feats)
                    await _publish(r, result)
            except Exception as exc:
                log.debug("subscriber loop ignored message: %s", exc)
    except asyncio.CancelledError:
        return
    except Exception as exc:
        log.warning("subscriber crashed: %s", exc)


app = make_app()
