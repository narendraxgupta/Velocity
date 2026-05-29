"""Benchmarks resource — POST /v1/benchmarks and watch streaming."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Iterator, Literal, TYPE_CHECKING

Profile = Literal[
    "baseline",
    "spike",
    "fire-hose",
    "adversarial",
    "cliff-finder",
    "cross-venue",
]


@dataclass
class LatencyBucket:
    p50: int = 0
    p90: int = 0
    p99: int = 0
    p999: int = 0
    max: int = 0

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "LatencyBucket":
        return cls(
            p50=d.get("p50", 0),
            p90=d.get("p90", 0),
            p99=d.get("p99", 0),
            p999=d.get("p999", 0),
            max=d.get("max", 0),
        )


@dataclass
class Benchmark:
    id: str
    submission_id: str
    profile: Profile
    started_at_ns: int

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Benchmark":
        return cls(
            id=d["id"],
            submission_id=d["submission_id"],
            profile=d["profile"],
            started_at_ns=d.get("started_at_ns", 0),
        )


@dataclass
class WatchEvent:
    benchmark_id: str
    phase: str
    rps: float
    latency_ns: LatencyBucket
    score: float
    ts_ns: int

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "WatchEvent":
        return cls(
            benchmark_id=d.get("benchmark_id", ""),
            phase=d.get("phase", ""),
            rps=float(d.get("rps", 0.0)),
            latency_ns=LatencyBucket.from_dict(d.get("latency_ns", {})),
            score=float(d.get("score", 0.0)),
            ts_ns=int(d.get("ts_ns", 0)),
        )


if TYPE_CHECKING:
    from .client import VelocityClient


class BenchmarksResource:
    def __init__(self, client: "VelocityClient") -> None:
        self._client = client

    def start(self, submission_id: str, profile: Profile) -> Benchmark:
        resp = self._client.request("POST", "/v1/benchmarks", body={
            "submission_id": submission_id, "profile": profile,
        })
        return Benchmark.from_dict(resp["benchmark"])

    def cancel(self, benchmark_id: str) -> None:
        self._client.request("POST", f"/v1/benchmarks/{benchmark_id}/cancel")

    def watch(self, benchmark_id: str) -> Iterator[WatchEvent]:
        for ev in self._client.stream(f"/v1/benchmarks/{benchmark_id}/watch"):
            yield WatchEvent.from_dict(ev)
