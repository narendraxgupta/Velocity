"""Leaderboard resource — GET /v1/leaderboard."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, TYPE_CHECKING


@dataclass
class LeaderboardEntry:
    rank: int
    submission_id: str
    team: str
    display: str
    composite_score: float
    latency_score: float
    throughput_score: float
    profile: str
    updated_at_ns: int

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "LeaderboardEntry":
        return cls(
            rank=d.get("rank", 0),
            submission_id=d["submission_id"],
            team=d.get("team", ""),
            display=d.get("display", ""),
            composite_score=float(d.get("composite_score", 0.0)),
            latency_score=float(d.get("latency_score", 0.0)),
            throughput_score=float(d.get("throughput_score", 0.0)),
            profile=d.get("profile", "baseline"),
            updated_at_ns=int(d.get("updated_at_ns", 0)),
        )


if TYPE_CHECKING:
    from .client import VelocityClient


class LeaderboardResource:
    def __init__(self, client: "VelocityClient") -> None:
        self._client = client

    def top(self, limit: int = 50) -> list[LeaderboardEntry]:
        resp = self._client.request("GET", f"/v1/leaderboard?limit={limit}")
        return [LeaderboardEntry.from_dict(e) for e in resp.get("entries", [])]
