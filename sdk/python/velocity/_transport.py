"""Internal request plumbing shared by the sync and async clients.

Both clients use httpx — sync uses httpx.Client, async uses
httpx.AsyncClient. The helpers here keep the request shape (headers,
retry, error decoding) in one place.
"""

from __future__ import annotations

import json
import random
import time
from dataclasses import dataclass
from typing import Any, Mapping

import httpx

from .errors import VelocityApiError


@dataclass(frozen=True)
class RequestSpec:
    method: str
    path: str
    body: Any = None
    extra_headers: Mapping[str, str] | None = None


def build_headers(bearer: str | None, user_agent: str,
                  extra: Mapping[str, str] | None) -> dict[str, str]:
    h: dict[str, str] = {
        "Accept": "application/json",
        "User-Agent": user_agent,
    }
    if bearer:
        h["Authorization"] = f"Bearer {bearer}"
    if extra:
        h.update(extra)
    return h


def serialize_body(body: Any) -> tuple[bytes | None, dict[str, str]]:
    """Return (raw_body, extra_headers) for the given request body.

    JSON for dict/list, raw bytes pass-through for bytes-like, str
    falls through as-is, files are read once.
    """
    if body is None:
        return None, {}
    if isinstance(body, (bytes, bytearray)):
        return bytes(body), {}
    if isinstance(body, str):
        return body.encode("utf-8"), {}
    if hasattr(body, "read"):
        return body.read(), {}
    return json.dumps(body, separators=(",", ":")).encode("utf-8"), \
           {"Content-Type": "application/json"}


def decode_response(res: httpx.Response) -> Any:
    if res.status_code >= 400:
        try:
            parsed = res.json()
        except Exception:  # noqa: BLE001
            parsed = res.text
        message = parsed.get("error") if isinstance(parsed, dict) else parsed
        raise VelocityApiError(res.status_code, str(message), parsed)
    ctype = res.headers.get("content-type", "")
    if "application/json" in ctype:
        return res.json()
    return res.text


def backoff_seconds(attempt: int) -> float:
    # Same shape as the Go and TS SDKs: capped exponential + jitter.
    return min(1.0, (1 << attempt) * 0.1 + random.random() * 0.05)


def sleep_sync(seconds: float) -> None:
    time.sleep(seconds)
