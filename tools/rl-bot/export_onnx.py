"""Export a trained SB3 PPO model to a flat ONNX graph the bot-worker
can consume.

We do NOT export the full SB3 policy (which includes value head, log
std, distribution machinery). The runtime only needs:

  input:  (1, 12) float32  — RLObservation
  outputs:
    (1, 3) float32  — discrete action logits (POST_BID / POST_ASK / CANCEL)
    (1, 2) float32  — continuous controls (spread_ticks_scale, size_scale)

Because SB3's Box(5) action is split (3 discrete logits + 2 continuous
heads in our convention), we wrap the policy in a tiny nn.Module that
projects the final MLP layer to those two outputs.
"""

from __future__ import annotations

import argparse
import pathlib

import torch
import torch.nn as nn
from stable_baselines3 import PPO


class ExportedPolicy(nn.Module):
    """Strips the PPO policy down to its inference path."""

    def __init__(self, mlp_extractor: nn.Module, action_net: nn.Module):
        super().__init__()
        self.mlp_extractor = mlp_extractor
        self.action_net = action_net

    def forward(self, x: torch.Tensor):
        latent = self.mlp_extractor.policy_net(x)
        out = self.action_net(latent)  # shape (B, 5)
        # During training SB3 clips the Box action to the action_space
        # bounds ([-5, 5] for the three discrete logits) BEFORE the env
        # takes argmax. If we export the raw logits, two confident logits
        # that both exceed +5 are flattened to a tie at train time (argmax
        # picks the first) but stay distinct at serve time — flipping the
        # chosen action. Clamp here so the exported graph's argmax matches
        # what the policy actually saw while learning.
        logits = torch.clamp(out[..., :3], -5.0, 5.0)
        spread_ticks = torch.clamp(out[..., 3:4] * 8.0, 0.0, 8.0)
        size_scale = torch.clamp(out[..., 4:5], 0.0, 1.0)
        controls = torch.cat([spread_ticks, size_scale], dim=-1)
        return logits, controls


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--in",  dest="inp", type=pathlib.Path, required=True,
                        help="Path to the trained model.zip from train.py.")
    parser.add_argument("--out", type=pathlib.Path, required=True,
                        help="Output ONNX file path.")
    args = parser.parse_args()

    model = PPO.load(str(args.inp), device="cpu")
    policy = model.policy
    policy.set_training_mode(False)

    exported = ExportedPolicy(policy.mlp_extractor, policy.action_net)
    exported.eval()

    dummy = torch.zeros(1, 12, dtype=torch.float32)
    torch.onnx.export(
        exported,
        dummy,
        str(args.out),
        input_names=["obs"],
        output_names=["action_logits", "action_controls"],
        dynamic_axes={
            "obs": {0: "batch"},
            "action_logits": {0: "batch"},
            "action_controls": {0: "batch"},
        },
        opset_version=17,
    )
    print(f"Exported ONNX: {args.out}")


if __name__ == "__main__":
    main()
