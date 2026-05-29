# Velocity — Scoring Methodology

> *How a submission goes from a stream of orders to a number on the leaderboard.*

This document defines exactly how the composite score is computed.
Anyone running a submission is entitled to know — and reproduce —
every penalty.

---

## 1. The Composite Score

A submission's composite score is a weighted blend of three sub-scores:

$$
S_{\text{composite}} = 0.40 \cdot S_{\text{throughput}} + 0.35 \cdot S_{\text{latency}} + 0.25 \cdot S_{\text{correctness}} - P
$$

Where each $S_x \in [0, 100]$ and $P$ is the total penalty (see §5).

**Why these weights?** A trading system that is fast but wrong is worthless.
A correct system that is slow is also worthless. A correct, fast system that
cannot sustain peak load is a liability. The weighting reflects the priority
order: throughput → latency → correctness, with correctness as a near-veto via
penalties.

The final ranking key in Redis is `composite_score` to 4 decimal places; ties
break on raw p99 (lower wins), then on submission timestamp (earlier wins).

---

## 2. Throughput Score

Let `target_rps` be the headline RPS for the benchmark profile and
`sustained_rps` be the **p10 of the 1-second TPS samples** during the hold
phase (we use p10, not mean, because what matters is the *worst* sustained
throughput, not the average).

$$
S_{\text{throughput}} =
    100 \cdot \min\!\left(1, \frac{\text{sustained\_rps}}{\text{target\_rps}}\right)
$$

Capping at 100 prevents over-fitting; a submission that handles 2× target
doesn't beat one that handles 1×. They're both "fast enough."

---

## 3. Latency Score

We use **p99 latency** because tail latency is what hurts in trading. p50 is
trivia.

Let `p99_us` be the observed p99 latency (microseconds) over the hold phase,
computed from the merged `HdrHistogram` across all bot workers.
Let `baseline_us` be the platform's measured *self-latency* — the latency
between two of our own services with no submission in the loop. This is
typically ~30 µs.

$$
S_{\text{latency}} =
\begin{cases}
100 & \text{if } p99\_us \leq baseline\_us \\
\max(0, 100 - 100 \cdot \frac{p99\_us - baseline\_us}{baseline\_us}) & \text{otherwise}
\end{cases}
$$

That is: a submission that matches our own service-to-service latency scores
100. Every additional `baseline_us` of latency costs 100 points. So at
roughly 2× baseline (≈60µs), the score is 0.

Note that this is *deliberately* punishing — at this level of perf
engineering, an extra microsecond is a real choice.

---

## 4. Correctness Score

The validator replays the bot's order stream against a reference orderbook
and produces three counts per submission:

- `expected_fills`  — what the reference book would have filled
- `actual_fills`    — what the submission claimed to fill
- `correct_fills`   — fills present in both with matching `(price, quantity, taker, maker)`

$$
S_{\text{correctness}} = 100 \cdot \frac{\text{correct\_fills}}{\max(1, \text{expected\_fills})}
$$

We do not punish *over-filling* in this number — that is what the **phantom
fill penalty** is for (§5).

### Violation taxonomy

The validator additionally tracks structural violations:

| Violation | Definition | Penalty point cost |
|-----------|------------|--------------------|
| **Price violation** | Reported fill price is *better* than the best book price at the time | 5 each |
| **Priority violation** | At the same price level, a later order filled before an earlier one | 3 each |
| **Phantom fill** | Reported a fill with no corresponding pair of orders | 5 each |
| **Missing fill** | Should have filled but did not (after generous timeout) | 2 each |
| **Self-cross** | Filled an order against the same client's other order | 10 each |

These accumulate into the penalty term `P` (capped at 50 points total to avoid
crushing the entire score on a single broken edge case).

---

## 5. Penalties

```text
P = min(50,   penalty_from_violations
            + penalty_from_lifecycle
            + penalty_from_resource_breach)
```

### Lifecycle penalties

| Event | Points |
|-------|--------|
| OOM-killed | 25 |
| Pod crashed during benchmark | 50 (entire score zeroed in practice) |
| Health probe failed at start | Disqualified |
| Health probe failed mid-run | 15 |

### Resource breach penalties

| Event | Points |
|-------|--------|
| Exceeded CPU quota momentarily | 0 (cgroups handles this transparently) |
| Exceeded memory soft limit (within hard cap) | 5 |
| Filesystem write outside `/tmp` | 5 |
| Network egress attempt | 10 |

---

## 6. Why p10, not mean, for sustained RPS?

Imagine two submissions both averaging 100k RPS:

- **A**: a flat line at 100k.
- **B**: 200k for 10s then 0 for 10s.

By mean, they tie. In trading, B is a disaster — half the time the exchange
is dead. By p10, A gets ~100k and B gets ~0. We rank A higher. This matches
what an SRE would call the "service level objective" view of throughput.

---

## 7. Benchmark profiles

The benchmark profile selected at `StartBenchmark` time determines
`target_rps`, ramp, hold, persona mix, and per-order timeout. Default
profiles live in `infra/profiles/`:

- **`baseline.yaml`** — 50k target, 30s hold, mostly market makers.
- **`spike.yaml`** — 200k target, 5s ramp, mixed personas, tests burst handling.
- **`fire-hose.yaml`** — 1M target, 60s hold, intentionally over-provisioned
  load to find the breaking point.
- **`adversarial.yaml`** — Heavy spoofer + canceller mix, tests cancel-path
  performance specifically.

The leaderboard composite score is the **mean of the composite scores across
all four profiles** weighted equally. This prevents over-fitting to one
workload.

---

## 8. Worked example

A submission, running the `baseline` profile:

| Metric | Observed |
|--------|----------|
| target_rps | 50,000 |
| sustained_rps (p10 of 1s samples) | 47,500 |
| p99 latency | 84 µs |
| baseline self-latency | 30 µs |
| expected_fills | 12,400 |
| correct_fills | 12,398 |
| priority violations | 2 |
| OOM killed | no |

Sub-scores:

```
S_throughput  = 100 * min(1, 47500/50000)            = 95.00
S_latency     = max(0, 100 - 100*(84-30)/30)         = 100 - 180 → 0.00
S_correctness = 100 * 12398/12400                    = 99.98
P             = 2 * 3                                 = 6.00
```

Composite:

```
S = 0.40*95.00 + 0.35*0.00 + 0.25*99.98 - 6.00
  = 38.00 + 0.00 + 24.995 - 6.00
  = 56.99
```

Note how badly the latency component punishes us — p99 of 84µs against a 30µs
baseline is "off by 2.8× the baseline" which by our formula zeroes the
latency sub-score. This is intentional. If you want a higher score, get
closer to baseline.

---

## 9. Stability and adversarial resistance

- **Determinism**: same RNG seed + same target system = same scores within
  ±0.1 composite points across runs.
- **No self-influence**: the platform's measurement overhead is calibrated
  against a no-op baseline submission and subtracted (see `baseline_us`
  above).
- **Adversarial detection**: a submission that returns `200 OK` to every
  request without doing real work gets caught by the correctness validator
  (no fills, large `missing_fills` count → near-zero correctness score).
