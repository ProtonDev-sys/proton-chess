from __future__ import annotations

import argparse
from pathlib import Path

try:
    import torch
except ImportError:  # pragma: no cover
    torch = None

if __package__ in {None, ""}:
    import sys

    sys.path.append(str(Path(__file__).resolve().parents[1]))
    from training.models import CoreEvalNet, DeepEvalNet
else:
    from .models import CoreEvalNet, DeepEvalNet


def export_model(model_name: str, output_path: Path) -> None:
    if torch is None:
        raise RuntimeError("PyTorch is required to export ONNX models.")

    if model_name == "core":
        model = CoreEvalNet()
        dummy = torch.zeros(1, 768)
        outputs = ["value"]
    elif model_name == "deep":
        model = DeepEvalNet()
        dummy = torch.zeros(1, 768)
        outputs = ["value", "policy"]
    else:
        raise ValueError(f"unknown model {model_name}")

    model.eval()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(
        model,
        dummy,
        output_path,
        input_names=["features"],
        output_names=outputs,
        dynamic_axes={"features": {0: "batch"}},
        opset_version=17,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Export Proton Chess models to ONNX.")
    parser.add_argument("model", choices=["core", "deep"])
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    export_model(args.model, args.output)


if __name__ == "__main__":
    main()
