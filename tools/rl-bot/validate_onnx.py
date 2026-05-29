"""Sanity-check an exported policy.onnx.

Loads the model with onnxruntime, runs 1000 random observations, and
prints summary stats. Useful both as a smoke test post-export and as
copy-paste documentation for what the bot-worker is doing in C++.
"""

from __future__ import annotations

import argparse
import time

import numpy as np
import onnxruntime as ort


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--n", type=int, default=1000)
    args = parser.parse_args()

    sess = ort.InferenceSession(args.model, providers=["CPUExecutionProvider"])
    in_name = sess.get_inputs()[0].name
    out_names = [o.name for o in sess.get_outputs()]
    print(f"Input: {in_name}  shape={sess.get_inputs()[0].shape}")
    for o in sess.get_outputs():
        print(f"Output: {o.name}  shape={o.shape}")

    rng = np.random.default_rng(0)
    obs = rng.standard_normal((args.n, 12)).astype(np.float32) * 0.5

    # Warm-up.
    for i in range(10):
        sess.run(out_names, {in_name: obs[i:i + 1]})

    start = time.perf_counter_ns()
    logits_acc = np.zeros(3)
    for i in range(args.n):
        logits, controls = sess.run(out_names, {in_name: obs[i:i + 1]})
        logits_acc += logits[0]
    elapsed = (time.perf_counter_ns() - start) / args.n
    print(f"Mean inference latency: {elapsed/1000:.2f} µs / call ({args.n} calls)")
    print(f"Action distribution (argmax of mean logits): {np.argmax(logits_acc / args.n)}")


if __name__ == "__main__":
    main()
