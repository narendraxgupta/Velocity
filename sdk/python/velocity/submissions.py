"""Submissions resource — POST /v1/submissions and friends."""

from __future__ import annotations

from dataclasses import dataclass
from typing import IO, Any, Literal, TYPE_CHECKING

import httpx

SubmissionKind = Literal["matching_engine", "market_maker", "pcap_replay"]


@dataclass
class Submission:
    id: str
    team: str
    display: str
    kind: SubmissionKind
    status: str
    created_at_ns: int

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Submission":
        return cls(
            id=d["id"],
            team=d["team"],
            display=d["display"],
            kind=d["kind"],
            status=d.get("status", "pending"),
            created_at_ns=d.get("created_at_ns", 0),
        )


if TYPE_CHECKING:
    from .client import VelocityClient


class SubmissionsResource:
    def __init__(self, client: "VelocityClient") -> None:
        self._client = client

    def create(
        self,
        team: str,
        display: str,
        kind: SubmissionKind,
        source: IO[bytes] | bytes | None = None,
    ) -> Submission:
        resp = self._client.request("POST", "/v1/submissions", body={
            "team": team, "display": display, "kind": kind,
        })
        submission = Submission.from_dict(resp["submission"])
        upload_url = resp.get("upload_url")
        if source is not None and upload_url:
            body = source.read() if hasattr(source, "read") else source
            r = httpx.put(upload_url, content=body)
            r.raise_for_status()
            self._client.request("POST",
                f"/v1/submissions/{submission.id}/uploaded",
                body={"id": submission.id})
        return submission

    def get(self, submission_id: str) -> Submission:
        resp = self._client.request("GET", f"/v1/submissions/{submission_id}")
        return Submission.from_dict(resp["submission"])

    def list(self, limit: int = 20) -> list[Submission]:
        resp = self._client.request("GET", f"/v1/submissions?limit={limit}")
        return [Submission.from_dict(s) for s in resp.get("submissions", [])]
