# Velocity Python SDK

> **Preview:** not yet integration-tested against the gateway — verify
> request/response shapes against `services/api-gateway/src/routes/*` and
> `proto/`. See `sdk/README.md` for the full caveat.

```bash
pip install velocity-sdk
```

```python
from velocity import VelocityClient

client = VelocityClient(
    base_url="https://demo.velocityhq.io",
    bearer=os.environ["VELOCITY_TOKEN"],
)

submission = client.submissions.create(
    team="acme",
    display="low-latency-matching-v3",
    kind="matching_engine",
    source=open("dist/exchange.tar.gz", "rb"),
)

benchmark = client.benchmarks.start(submission.id, profile="baseline")

for event in client.benchmarks.watch(benchmark.id):
    print(f"phase={event.phase} p99={event.latency_ns.p99}ns")
```

## Async

Every method also exists in async form on `AsyncVelocityClient`:

```python
async with AsyncVelocityClient(base_url=..., bearer=...) as client:
    async for event in client.benchmarks.watch(benchmark.id):
        ...
```

## Errors

* `VelocityApiError` — non-2xx response. `.status` and `.body` available.
* `VelocityNetworkError` — DNS / TCP / abort.
