"""Synchronous VelocityClient.

The synchronous client is the default — it's what you want from a Jupyter
notebook, a benchmarking script, or any CLI tool. For long-lived
servers or async frameworks (FastAPI, anyio), use ``AsyncVelocityClient``
from ``velocity.async_client``.
"""

from __future__ import annotations

import json
from typing import Any, Iterator, Mapping

import httpx

from . import _transport as t
from .errors import VelocityApiError, VelocityNetworkError
from .submissions import SubmissionsResource
from .benchmarks import BenchmarksResource
from .leaderboard import LeaderboardResource
from .audit import AuditResource


class VelocityClient:
    def __init__(
        self,
        base_url: str,
        bearer: str | None = None,
        *,
        user_agent: str = "velocity-sdk-py/0.1",
        timeout: float = 30.0,
        max_retries: int = 2,
    ) -> None:
        if not base_url:
            raise ValueError("base_url is required")
        self.base_url    = base_url.rstrip("/")
        self.bearer      = bearer
        self.user_agent  = user_agent
        self.max_retries = max_retries
        self._http = httpx.Client(timeout=timeout, follow_redirects=False)

        self.submissions = SubmissionsResource(self)
        self.benchmarks  = BenchmarksResource(self)
        self.leaderboard = LeaderboardResource(self)
        self.audit       = AuditResource(self)

    # -- context manager ----------------------------------------------------

    def __enter__(self) -> "VelocityClient":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        self._http.close()

    # -- low-level request --------------------------------------------------

    def request(
        self,
        method: str,
        path: str,
        body: Any = None,
        *,
        extra_headers: Mapping[str, str] | None = None,
    ) -> Any:
        raw, content_headers = t.serialize_body(body)
        headers = t.build_headers(self.bearer, self.user_agent, extra_headers)
        headers.update(content_headers)

        last_exc: BaseException | None = None
        for attempt in range(self.max_retries + 1):
            try:
                res = self._http.request(method,
                                         f"{self.base_url}{path}",
                                         content=raw, headers=headers)
            except httpx.HTTPError as e:
                last_exc = e
                if attempt == self.max_retries:
                    raise VelocityNetworkError(str(e), e) from e
                t.sleep_sync(t.backoff_seconds(attempt))
                continue

            if res.status_code >= 500 and attempt < self.max_retries:
                t.sleep_sync(t.backoff_seconds(attempt))
                continue
            return t.decode_response(res)

        # Unreachable in practice — loop always returns or raises.
        raise VelocityNetworkError("max retries exceeded", last_exc)

    # -- streaming ----------------------------------------------------------

    def stream(self, path: str) -> Iterator[Any]:
        """Yield decoded JSON event-data from an SSE endpoint."""
        headers = t.build_headers(self.bearer, self.user_agent, None)
        headers["Accept"] = "text/event-stream"
        with self._http.stream("GET", f"{self.base_url}{path}",
                               headers=headers, timeout=None) as res:
            if res.status_code >= 400:
                text = res.read().decode("utf-8", errors="replace")
                try:
                    parsed = json.loads(text)
                except json.JSONDecodeError:
                    parsed = text
                raise VelocityApiError(res.status_code, str(parsed), parsed)
            buffer = ""
            for chunk in res.iter_text():
                buffer += chunk
                while "\n\n" in buffer:
                    frame, buffer = buffer.split("\n\n", 1)
                    data = []
                    for line in frame.splitlines():
                        if line.startswith("data:"):
                            data.append(line[5:].strip())
                    if not data:
                        continue
                    try:
                        yield json.loads("\n".join(data))
                    except json.JSONDecodeError:
                        # Skip malformed frames — server may emit
                        # `:keepalive` comments which we ignore.
                        continue
