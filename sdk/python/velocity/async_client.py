"""Asynchronous AsyncVelocityClient.

Mirror of VelocityClient using httpx.AsyncClient. Resource classes
poke at both `request()` and `stream()` so the same resource code can
be reused; for now the resource classes only know about the sync
client, so we instantiate sync-flavoured resources here too — async
callers should use the methods on this class directly.
"""

from __future__ import annotations

import json
from typing import Any, AsyncIterator, Mapping

import httpx

from . import _transport as t
from .errors import VelocityApiError, VelocityNetworkError


class AsyncVelocityClient:
    def __init__(
        self,
        base_url: str,
        bearer: str | None = None,
        *,
        user_agent: str = "velocity-sdk-py-async/0.1",
        timeout: float = 30.0,
        max_retries: int = 2,
    ) -> None:
        if not base_url:
            raise ValueError("base_url is required")
        self.base_url    = base_url.rstrip("/")
        self.bearer      = bearer
        self.user_agent  = user_agent
        self.max_retries = max_retries
        self._http: httpx.AsyncClient | None = None
        self._timeout = timeout

    async def __aenter__(self) -> "AsyncVelocityClient":
        self._http = httpx.AsyncClient(timeout=self._timeout, follow_redirects=False)
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.aclose()

    async def aclose(self) -> None:
        if self._http is not None:
            await self._http.aclose()
            self._http = None

    async def request(
        self,
        method: str,
        path: str,
        body: Any = None,
        *,
        extra_headers: Mapping[str, str] | None = None,
    ) -> Any:
        if self._http is None:
            raise RuntimeError("AsyncVelocityClient must be used as `async with` context manager")
        raw, content_headers = t.serialize_body(body)
        headers = t.build_headers(self.bearer, self.user_agent, extra_headers)
        headers.update(content_headers)

        import asyncio

        last_exc: BaseException | None = None
        for attempt in range(self.max_retries + 1):
            try:
                res = await self._http.request(method,
                                               f"{self.base_url}{path}",
                                               content=raw, headers=headers)
            except httpx.HTTPError as e:
                last_exc = e
                if attempt == self.max_retries:
                    raise VelocityNetworkError(str(e), e) from e
                await asyncio.sleep(t.backoff_seconds(attempt))
                continue

            if res.status_code >= 500 and attempt < self.max_retries:
                await asyncio.sleep(t.backoff_seconds(attempt))
                continue
            return t.decode_response(res)
        raise VelocityNetworkError("max retries exceeded", last_exc)

    async def stream(self, path: str) -> AsyncIterator[Any]:
        if self._http is None:
            raise RuntimeError("AsyncVelocityClient must be used as `async with` context manager")
        headers = t.build_headers(self.bearer, self.user_agent, None)
        headers["Accept"] = "text/event-stream"

        async with self._http.stream("GET", f"{self.base_url}{path}",
                                     headers=headers, timeout=None) as res:
            if res.status_code >= 400:
                text = (await res.aread()).decode("utf-8", errors="replace")
                try:
                    parsed = json.loads(text)
                except json.JSONDecodeError:
                    parsed = text
                raise VelocityApiError(res.status_code, str(parsed), parsed)
            buffer = ""
            async for chunk in res.aiter_text():
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
                        continue
