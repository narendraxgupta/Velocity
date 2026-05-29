"""Synthetic market environment for training the adaptive bot persona.

The environment is intentionally simple: it must run thousands of
steps/second across many parallel envs during PPO training and the
exact dynamics aren't the point — what matters is that the *features*
the trained policy sees during training are identical to those the
bot-worker constructs at inference time.

That tight coupling lives in ``Observation`` below. Any change to the
observation shape needs to be mirrored to
``services/bot-fleet/worker/include/bot_worker/rl_policy.h``.

Dynamics
--------
- Mid price: discretised OU process (mean-reverting around 100.0).
- News shocks: small Poisson process of jumps; each jump perturbs the
  mid by N(0, 0.3) in display units.
- Spread / book imbalance: drawn from independent stationary
  distributions calibrated to mid liquidity venues.
- Bot order outcomes:
    * Resting BID/ASK fills probabilistically based on how close to
      mid they are AND inverse of the spread.
    * Hold inventory → mark-to-market PnL when mid moves.

Reward
------
    r_t = ΔPnL_t  − λ · |position|  − μ · |trade_qty|

with λ = 0.0005, μ = 0.0002. The penalties keep the agent from
discovering that "post 1k contracts at fair and don't cancel" is a
zero-risk strategy in this toy env.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
import gymnasium as gym
from gymnasium import spaces


# Match the C++ enum exactly — order matters.
ACTION_POST_BID = 0
ACTION_POST_ASK = 1
ACTION_CANCEL = 2


@dataclass
class EnvConfig:
    horizon_steps: int = 1000          # steps per episode
    tick_size: float = 0.01            # display units
    ou_theta: float = 0.5
    ou_mu: float = 100.0
    ou_sigma: float = 0.05
    jump_rate_per_step: float = 0.005
    jump_std: float = 0.30
    inventory_penalty: float = 5e-4
    turnover_penalty: float = 2e-4
    max_spread_ticks: float = 8.0
    base_qty: float = 10.0


class VelocityMarketEnv(gym.Env):
    """Single-symbol order-book proxy with 12-feature observations."""

    metadata = {"render_modes": []}

    def __init__(self, config: EnvConfig | None = None, seed: int | None = None):
        super().__init__()
        self.cfg = config or EnvConfig()
        self.rng = np.random.default_rng(seed)

        # Observation: 12 floats. Order MUST match bot_worker/rl_policy.h.
        self.observation_space = spaces.Box(
            low=-10.0, high=10.0, shape=(12,), dtype=np.float32,
        )

        # Action: (discrete kind, continuous spread_ticks, continuous size_scale).
        # SB3's PPO needs a flat box; we encode the discrete via softmax over
        # the first 3 dims and use the last two as continuous controls. The
        # network head is then a 5-dim (logits[3] | tanh[2]) output.
        self.action_space = spaces.Box(
            low=np.array([-5, -5, -5, 0.0, 0.0], dtype=np.float32),
            high=np.array([5, 5, 5, 1.0, 1.0], dtype=np.float32),
            dtype=np.float32,
        )

        self._reset_internal()

    # --------------- gym API ----------------------------------------------

    def reset(self, *, seed: int | None = None, options=None):
        super().reset(seed=seed)
        if seed is not None:
            self.rng = np.random.default_rng(seed)
        self._reset_internal()
        return self._obs(), {}

    def step(self, action: np.ndarray):
        kind = int(np.argmax(action[:3]))
        spread_ticks = float(np.clip(action[3] * self.cfg.max_spread_ticks,
                                     0.0, self.cfg.max_spread_ticks))
        size_scale = float(np.clip(action[4], 0.0, 1.0))

        # ----- advance market -------------------------------------------
        dt = 1.0
        z = self.rng.standard_normal()
        decay = math.exp(-self.cfg.ou_theta * dt)
        std = self.cfg.ou_sigma * math.sqrt((1 - math.exp(-2 * self.cfg.ou_theta * dt)) /
                                            (2 * self.cfg.ou_theta))
        new_mid = self.mid * decay + self.cfg.ou_mu * (1 - decay) + std * z
        if self.rng.random() < self.cfg.jump_rate_per_step:
            new_mid += self.rng.normal(0.0, self.cfg.jump_std)
        d_mid = new_mid - self.mid
        self.mid = max(new_mid, self.cfg.tick_size)

        # ----- order execution ------------------------------------------
        traded_qty = 0.0
        d_pnl_unrealized = self.position * d_mid

        if kind == ACTION_POST_BID:
            self.last_post_step = self.step_idx
            fill_prob = math.exp(-spread_ticks * 0.3) * size_scale
            if self.rng.random() < fill_prob:
                qty = self.cfg.base_qty * size_scale
                fill_price = self.mid - spread_ticks * self.cfg.tick_size
                self.position += qty
                self.cash -= qty * fill_price
                traded_qty += qty
                self.recent_fills += 1
        elif kind == ACTION_POST_ASK:
            self.last_post_step = self.step_idx
            fill_prob = math.exp(-spread_ticks * 0.3) * size_scale
            if self.rng.random() < fill_prob:
                qty = self.cfg.base_qty * size_scale
                fill_price = self.mid + spread_ticks * self.cfg.tick_size
                self.position -= qty
                self.cash += qty * fill_price
                traded_qty += qty
                self.recent_fills += 1
        else:  # CANCEL — frees up any inventory bias; tiny reward for risk.
            self.recent_cancels += 1

        # Decay rate-counters so they look like a 100ms window.
        self.recent_fills *= 0.7
        self.recent_cancels *= 0.7

        # ----- reward ----------------------------------------------------
        reward = (
            d_pnl_unrealized
            - self.cfg.inventory_penalty * abs(self.position)
            - self.cfg.turnover_penalty * traded_qty
        )

        # Track recent mid for trend/vol features.
        self._mid_history.append(self.mid)
        if len(self._mid_history) > 32:
            self._mid_history.pop(0)

        self.step_idx += 1
        terminated = False
        truncated = self.step_idx >= self.cfg.horizon_steps
        info = {
            "position": self.position,
            "cash": self.cash,
            "mid": self.mid,
        }
        return self._obs(), float(reward), terminated, truncated, info

    # --------------- private ----------------------------------------------

    def _reset_internal(self):
        self.mid = self.cfg.ou_mu + self.rng.normal(0.0, 0.2)
        self.position = 0.0
        self.cash = 0.0
        self.recent_fills = 0.0
        self.recent_cancels = 0.0
        self.step_idx = 0
        self.last_post_step = 0
        self._mid_history: list[float] = [self.mid]
        self._vol_ema = 0.5

    def _obs(self) -> np.ndarray:
        # Replicates RLObservation layout (rl_policy.h).
        # Spread / book_imbalance / queue_position are synthesised here;
        # the real bot-worker doesn't see the submitter's book directly
        # so the trained policy must work from proxies.
        mids = self._mid_history
        trend_ticks = (mids[-1] - mids[0]) / self.cfg.tick_size if len(mids) > 1 else 0.0
        # EWMA absolute step size as a vol proxy.
        if len(mids) > 1:
            self._vol_ema = 0.92 * self._vol_ema + 0.08 * abs(mids[-1] - mids[-2]) / self.cfg.tick_size

        spread_ticks = float(np.clip(self.rng.normal(2.0, 0.5), 1.0, 32.0))
        book_imbalance = float(self.rng.uniform(-0.6, 0.6))
        queue_position = float(self.rng.uniform(0.0, 1.0))

        obs = np.array([
            math.log(max(self.mid, 1e-9)) / 100.0,
            self.position / 100.0,
            self.cash / 100.0,
            book_imbalance,
            spread_ticks,
            self.recent_fills / 5.0,
            self.recent_cancels / 5.0,
            queue_position,
            trend_ticks,
            self._vol_ema,
            min(float((self.step_idx - self.last_post_step) * 10), 5000.0),
            (self.step_idx % 1024) / 1024.0,
        ], dtype=np.float32)
        return obs
