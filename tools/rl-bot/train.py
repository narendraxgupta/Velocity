"""Train a PPO policy on VelocityMarketEnv.

This script is deliberately spartan: it uses SB3 defaults except for
the things that matter for our latency / inference profile (small
network, no batch norm, deterministic action sampling on eval).

Example:

    python train.py --steps 3_000_000 --out runs/2025-04-12
"""

from __future__ import annotations

import argparse
import os
import pathlib

import numpy as np
from stable_baselines3 import PPO
from stable_baselines3.common.callbacks import CheckpointCallback
from stable_baselines3.common.env_util import make_vec_env
from stable_baselines3.common.vec_env import SubprocVecEnv

from velocity_market_env import VelocityMarketEnv


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--steps", type=int, default=3_000_000,
                        help="Total environment steps for PPO.learn().")
    parser.add_argument("--n-envs", type=int, default=8,
                        help="Parallel envs for vectorised rollouts.")
    parser.add_argument("--out", type=pathlib.Path, required=True,
                        help="Output directory for the model + tensorboard logs.")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)

    # Two-layer MLP, 64 units each. Small enough that ORT inference is
    # ~1-2µs on a tuned x86 box; that's our per-bot budget.
    policy_kwargs = dict(net_arch=[64, 64])

    env = make_vec_env(
        VelocityMarketEnv,
        n_envs=args.n_envs,
        seed=args.seed,
        vec_env_cls=SubprocVecEnv if args.n_envs > 1 else None,
    )

    model = PPO(
        policy="MlpPolicy",
        env=env,
        learning_rate=3e-4,
        n_steps=2048,
        batch_size=256,
        gamma=0.995,
        gae_lambda=0.95,
        clip_range=0.2,
        ent_coef=0.01,
        vf_coef=0.5,
        max_grad_norm=0.5,
        policy_kwargs=policy_kwargs,
        verbose=1,
        seed=args.seed,
        tensorboard_log=str(args.out / "tb"),
    )

    ckpt_cb = CheckpointCallback(
        save_freq=max(50_000 // args.n_envs, 1),
        save_path=str(args.out / "checkpoints"),
        name_prefix="ppo",
    )

    model.learn(total_timesteps=args.steps, callback=ckpt_cb)
    model_path = args.out / "model.zip"
    model.save(str(model_path))
    print(f"Saved model: {model_path}")

    # Quick eval — average reward over 16 deterministic episodes.
    eval_env = VelocityMarketEnv(seed=args.seed + 1)
    returns: list[float] = []
    for ep in range(16):
        obs, _ = eval_env.reset(seed=args.seed + ep + 1)
        total = 0.0
        done = False
        while not done:
            action, _ = model.predict(obs, deterministic=True)
            obs, r, term, trunc, _ = eval_env.step(action)
            total += r
            done = term or trunc
        returns.append(total)
    print(f"Eval episode mean return: {np.mean(returns):.3f} ± {np.std(returns):.3f}")


if __name__ == "__main__":
    main()
