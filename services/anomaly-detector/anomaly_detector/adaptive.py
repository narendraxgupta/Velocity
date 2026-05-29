"""Adaptive benchmark profile generator.

Premise
-------
After a submission has been benchmarked at least once we know exactly
where its weak spots are. The cliff detector knows the throughput
ceiling; the regression detector knows whether the tail is moving; the
exec-quality panel knows whether slippage is bad. Instead of asking
the submitter to keep choosing profiles by hand we can synthesise the
*next* benchmark profile that's most likely to expose the worst
remaining weakness.

This is a pragmatic rules engine — not an RL policy or anything
fancier — because: (a) the input space is small (a dozen features),
(b) the output is a discrete profile, (c) interpretability matters
(the operator UI explains *why* it picked the profile).

Rule order (first match wins)
-----------------------------
  1. CLIFF DETECTED → "cliff-finder" so the next run pins the cliff
     more precisely.
  2. p999 REGRESSED → "tail-stress" (a longer hold at moderate RPS so
     coordinated omission can't hide tail blowups).
  3. HIGH SLIPPAGE → "adversarial" (heavy spoofer + canceller mix —
     drives the matcher into degenerate microstructure cases).
  4. LOW CORRECTNESS → "soak" (lower RPS, longer duration; lots of
     time for slow correctness drift to surface).
  5. ANOMALOUS HEALTH → "baseline" with extended hold; the
     IsolationForest verdict was noise → re-baseline.
  6. NO PRIOR DATA / OK → "fire-hose" because at OK health we want to
     keep pushing the ceiling.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any


@dataclass
class AdaptivePick:
    profile_name: str
    reason:       str
    inputs:       dict[str, Any]


def pick_profile(*, regression: dict | None, health: dict | None,
                 exec_quality: dict | None,
                 last_report: dict | None) -> AdaptivePick:
    inputs = {
        "regression":  regression,
        "health":      health,
        "exec_quality": exec_quality,
        "last_report": _summarise_report(last_report),
    }

    if last_report and last_report.get("cliff", {}).get("detected"):
        return AdaptivePick(
            profile_name="cliff-finder",
            reason=("cliff at "
                    f"{last_report['cliff']['rps']} req/s detected last run — "
                    "tightening the ladder to pin it"),
            inputs=inputs,
        )

    if regression and regression.get("regression") == "regressed":
        return AdaptivePick(
            profile_name="adversarial",
            reason=("p99 regressed by "
                    f"{regression['p99_delta_ns'] // 1000}µs (KS p={regression['ks_pvalue']:.2g}) — "
                    "stressing tail via adversarial profile"),
            inputs=inputs,
        )

    if exec_quality and float(exec_quality.get("slippage_bps", 0)) > 10:
        return AdaptivePick(
            profile_name="adversarial",
            reason=("slippage "
                    f"{exec_quality['slippage_bps']:.1f} bp exceeds 10bp threshold — "
                    "spoofer + canceller mix to expose matcher edge-cases"),
            inputs=inputs,
        )

    if last_report:
        correctness = last_report.get("scores", {}).get("correctness", 100.0)
        if correctness < 95.0:
            return AdaptivePick(
                profile_name="soak",
                reason=(f"correctness {correctness:.2f}% below 95% — "
                        "switching to long-duration soak to surface drift"),
                inputs=inputs,
            )

    if health and health.get("health") == "anomaly":
        return AdaptivePick(
            profile_name="baseline",
            reason=("IsolationForest flagged anomaly — "
                    "re-baselining with the steady profile"),
            inputs=inputs,
        )

    if not last_report:
        return AdaptivePick(
            profile_name="baseline",
            reason="no prior runs on record — starting from baseline",
            inputs=inputs,
        )

    return AdaptivePick(
        profile_name="fire-hose",
        reason=("no weak spots detected — pushing the throughput ceiling "
                "with fire-hose"),
        inputs=inputs,
    )


def _summarise_report(report: dict | None) -> dict | None:
    if report is None:
        return None
    return {
        "sustained_rps": report.get("sustained_rps"),
        "p99_ns":        report.get("latency", {}).get("p99_ns"),
        "p999_ns":       report.get("latency", {}).get("p999_ns"),
        "composite":     report.get("scores", {}).get("composite"),
        "correctness":   report.get("scores", {}).get("correctness"),
        "cliff_detected": (report.get("cliff") or {}).get("detected", False),
    }
