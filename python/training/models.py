from __future__ import annotations

try:
    import torch
    from torch import nn
except ImportError:  # pragma: no cover
    torch = None
    nn = object


if torch is not None:
    class CoreEvalNet(nn.Module):
        def __init__(self, input_dim: int = 768, hidden_dim: int = 256) -> None:
            super().__init__()
            self.model = nn.Sequential(
                nn.Linear(input_dim, hidden_dim),
                nn.ReLU(),
                nn.Linear(hidden_dim, hidden_dim),
                nn.ReLU(),
                nn.Linear(hidden_dim, 1),
            )

        def forward(self, x):
            return self.model(x)


    class DeepEvalNet(nn.Module):
        def __init__(self, input_dim: int = 768, hidden_dim: int = 512, policy_dim: int = 4096) -> None:
            super().__init__()
            self.trunk = nn.Sequential(
                nn.Linear(input_dim, hidden_dim),
                nn.ReLU(),
                nn.Linear(hidden_dim, hidden_dim),
                nn.ReLU(),
            )
            self.value_head = nn.Linear(hidden_dim, 1)
            self.policy_head = nn.Linear(hidden_dim, policy_dim)

        def forward(self, x):
            features = self.trunk(x)
            return self.value_head(features), self.policy_head(features)


else:
    class CoreEvalNet:  # pragma: no cover
        pass


    class DeepEvalNet:  # pragma: no cover
        pass
