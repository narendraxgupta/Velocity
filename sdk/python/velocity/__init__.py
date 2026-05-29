"""velocity-sdk — Python client for the Velocity benchmarking platform.

Public surface re-exports below. Resource classes live in submodules so
type checkers can find them on `from velocity.submissions import …`.
"""

from .client import VelocityClient
from .async_client import AsyncVelocityClient
from .errors import VelocityError, VelocityApiError, VelocityNetworkError
from .submissions import Submission, SubmissionKind
from .benchmarks import Benchmark, Profile, WatchEvent, LatencyBucket
from .leaderboard import LeaderboardEntry
from .audit import AuditEvent, AuditQuery

__all__ = [
    "VelocityClient",
    "AsyncVelocityClient",
    "VelocityError",
    "VelocityApiError",
    "VelocityNetworkError",
    "Submission",
    "SubmissionKind",
    "Benchmark",
    "Profile",
    "WatchEvent",
    "LatencyBucket",
    "LeaderboardEntry",
    "AuditEvent",
    "AuditQuery",
]

__version__ = "0.1.0"
