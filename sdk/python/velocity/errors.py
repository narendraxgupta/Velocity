"""Errors raised by the Velocity SDK.

Three concrete classes with a common base so callers can either catch
broadly (`except VelocityError`) or branch precisely on status codes.
"""

from __future__ import annotations

from typing import Any


class VelocityError(Exception):
    """Base class for all SDK errors."""


class VelocityApiError(VelocityError):
    """Raised for any non-2xx HTTP response.

    Attributes:
        status: HTTP status code returned by the gateway.
        body:   decoded response body (dict if JSON, str otherwise).
    """

    def __init__(self, status: int, message: str, body: Any) -> None:
        super().__init__(f"HTTP {status} — {message}")
        self.status = status
        self.body = body


class VelocityNetworkError(VelocityError):
    """Raised for transport-layer failures (DNS, connection, abort)."""

    def __init__(self, message: str, cause: BaseException | None = None) -> None:
        super().__init__(message)
        self.cause = cause
