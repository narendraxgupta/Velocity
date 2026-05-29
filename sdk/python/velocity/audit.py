"""Audit log resource — GET /v1/audit."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, TYPE_CHECKING
from urllib.parse import urlencode


@dataclass
class AuditEvent:
    event_id: str
    tenant_id: str
    subject: str
    role: str
    source: str
    action: str
    resource_type: str
    resource_id: str
    outcome: str
    status_code: int
    occurred_at_ns: int
    remote_ip: str
    request_id: str
    chain_hash: str = ""
    meta: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "AuditEvent":
        return cls(
            event_id=d.get("event_id", ""),
            tenant_id=d.get("tenant_id", ""),
            subject=d.get("subject", ""),
            role=d.get("role", ""),
            source=d.get("source", ""),
            action=d.get("action", ""),
            resource_type=d.get("resource_type", ""),
            resource_id=d.get("resource_id", ""),
            outcome=d.get("outcome", ""),
            status_code=int(d.get("status_code", 0)),
            occurred_at_ns=int(d.get("occurred_at_ns", 0)),
            remote_ip=d.get("remote_ip", ""),
            request_id=d.get("request_id", ""),
            chain_hash=d.get("chain_hash", ""),
            meta=d.get("meta", {}) or {},
        )


@dataclass
class AuditQuery:
    tenant: str | None = None
    action: str | None = None
    since: datetime | None = None
    until: datetime | None = None
    limit: int = 200

    def to_query_string(self) -> str:
        params: list[tuple[str, str]] = []
        if self.tenant:
            params.append(("tenant", self.tenant))
        if self.action:
            params.append(("action", self.action))
        if self.since:
            params.append(("since", _iso(self.since)))
        if self.until:
            params.append(("until", _iso(self.until)))
        params.append(("limit", str(self.limit)))
        return urlencode(params)


def _iso(dt: datetime) -> str:
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.isoformat().replace("+00:00", "Z")


if TYPE_CHECKING:
    from .client import VelocityClient


class AuditResource:
    def __init__(self, client: "VelocityClient") -> None:
        self._client = client

    def query(self, q: AuditQuery | None = None) -> list[AuditEvent]:
        q = q or AuditQuery()
        resp = self._client.request("GET", f"/v1/audit?{q.to_query_string()}")
        return [AuditEvent.from_dict(e) for e in resp.get("events", [])]
