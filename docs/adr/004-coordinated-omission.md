# ADR-004: Open-loop load with intended-send-time correction

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: Platform team

## Context

Naïve load generators measure latency like this:

```
for each request:
    t0 = now()
    send(req)
    wait(response)
    t1 = now()
    record(t1 - t0)
```

This is **closed-loop**: the next request waits for the previous one's
response. If the server slows down, the load generator also slows down. The
latencies recorded are correct *for the requests that the slow server allowed
through*, but the load generator silently stops sending the rest of the
requests that *should* have been in flight.

Gil Tene formalized this as **Coordinated Omission** and demonstrated that it
makes tail latencies look 10–100× better than reality. A server pausing for
1 second only adds a single 1-second sample to a closed-loop run, when in
reality it caused (target_rps × 1s) requests to back up.

For a benchmarking platform whose entire purpose is to publish *honest* tail
numbers, closed-loop measurement is disqualifying.

## Decision

`bot-fleet/worker` runs in **open-loop, constant-rate** mode. Pseudocode:

```
plan_send_time = start_time
for each bot in target_rps over duration:
    plan_send_time += 1 / target_rps
    sleep_until(plan_send_time)
    intended_ts = plan_send_time
    actual_ts = now()
    send(req)
    on response:
        ack_ts = now()
        record(ack_ts - intended_ts)        // <-- the critical line
```

If the server slows down so that `actual_ts > intended_ts`, we record the
*intended* send time, **not** the actual send time, in the latency
calculation. This way, when the server catches up, every backed-up request
is correctly attributed to the slow window.

The bots schedule sends with a **deadline-driven scheduler** built on
`io_uring`'s timeout opcodes; we do not use `sleep()` (which has its own
quantization issues).

## Consequences

### Good

- **Honest p99**. Our numbers match (or exceed in pessimism) what Gil's
  HdrHistogram philosophy demands.
- **Backpressure is observable, not hidden.** If the bot can't keep up with
  its own intended schedule, we emit a metric
  (`velocity_bot_schedule_skew_us`) instead of silently slowing down.

### Bad

- **The bot worker can fall behind the submission.** If the submission is
  fast and the bot is on a slow node, the bot's send queue grows. We cap
  this growth at 64k pending requests, after which we drop with a metric.
  Drops invalidate the benchmark for that worker (the controller reports
  the situation in the final BenchmarkReport).
- **Tuning matters.** `target_rps` is a *scheduled* rate, not an *achieved*
  rate. Setting target too high means we measure the bot's failure, not
  the submission's. The bot publishes its own utilization so operators
  can see this.

## Alternatives considered

### "Just record the raw latency and add a correction post hoc"

HdrHistogram supports `recordValueWithExpectedInterval()` which extrapolates
the missing samples. We use this **in addition** for cross-checking, but the
primary signal is the open-loop send schedule because extrapolation is a
weaker approximation than measuring the right thing in the first place.

### Use `wrk2` / `hey -c -d` style runners

`wrk2` is the gold standard for "Coordinated Omission-correct" HTTP load
testing. It is excellent. We do not use it directly because:

- We need protocol support beyond HTTP (WebSocket, FIX 4.4).
- We need *strict* control over RNG seeds for deterministic correctness
  validation.
- We need to attribute every request to a `submission_id` and publish to
  Redpanda — outside `wrk2`'s scope.

Our worker is conceptually `wrk2 + Redpanda producer + FIX dialect`.

### Closed-loop with a "warning" if response time exceeds 2× expected

This is what most enterprise load testers do. It is closer-to-honest than
naïve closed-loop, but it still suffers from the underlying coordination —
we choose to do it right rather than detect-and-warn.

## References

- ["Latency: Coordinated Omission" — Gil Tene, Strange Loop 2015](https://www.youtube.com/watch?v=lJ8ydIuPFeU)
- ["How NOT to Measure Latency"](https://www.infoq.com/presentations/latency-response-time/)
- [`wrk2` source](https://github.com/giltene/wrk2)
- [HdrHistogram documentation](https://github.com/HdrHistogram/HdrHistogram)
