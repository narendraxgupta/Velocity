# rl-bot — adaptive market-making persona

A small training package that produces the `policy.onnx` model the
`bot-worker` loads at startup for the `ADAPTIVE` persona.

## Why this is a separate package

Training runs on a workstation or a one-off cluster job — never inside
the platform's runtime path. The bot-worker only consumes the exported
ONNX graph. Keeping training out of `services/` makes the dependency
graph obvious:

```
tools/rl-bot/          → Python, PyTorch, stable-baselines3, OUR sim env
                ↓ exports
services/bot-fleet/worker/  →  ONNX Runtime, loads policy.onnx
```

## Quick start

```bash
cd platform/tools/rl-bot
pip install -r requirements.txt

# Trains a PPO market-maker on the synthetic OU + jump market env.
# ~3M steps, ~15 minutes on a CPU-only laptop with 8 envs in parallel.
python train.py --steps 3_000_000 --out runs/$(date +%Y%m%d-%H%M)

# Exports the latest run to ONNX with the shape (1,12) -> (1,3)+(1,2).
python export_onnx.py --in runs/<run>/model.zip --out policy.onnx

# Smoke-test the exported model.
python validate_onnx.py --model policy.onnx
```

Drop the produced `policy.onnx` at `/var/lib/velocity/rl/policy.onnx`
on each bot-worker (Helm chart mounts a ConfigMap-derived secret).

## Environment

`velocity_market_env.py` implements a single-symbol order-book sim
that mirrors the worker's surrogate inputs:

- mid price = OU process + Poisson jump shocks (matches
  `services/marketdata-generator`)
- 12-feature observation = the bot-worker's `RLObservation` layout
- Action = `(kind ∈ {POST_BID, POST_ASK, CANCEL}, spread_ticks ∈ [0,8],
  size_scale ∈ [0,1])`
- Reward = PnL − λ · |inventory| − μ · turnover
  (encourages flat books and avoids reward-hacking via wide quotes)

The reward weights are deliberately conservative; we'd rather train a
boring policy that beats `MARKET_MAKER` than a brilliant one that
discovers a way to game the sim.
