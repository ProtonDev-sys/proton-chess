"""Conservative one-sided confidence bounds for color-reversed opening pairs."""

import math


def score_bounds(points: list[float | None], confidence: float = 0.95) -> tuple[float, float]:
    if not points or len(points) % 2:
        raise ValueError("confidence bounds require complete opening pairs")
    if not 0 < confidence < 1:
        raise ValueError("confidence must be between zero and one")
    if any(point is not None and point not in (0.0, 0.5, 1.0) for point in points):
        raise ValueError("invalid game score")
    radius = math.sqrt(math.log(1 / (1 - confidence)) / (2 * (len(points) // 2)))
    lower_mean = sum(0 if point is None else point for point in points) / len(points)
    upper_mean = sum(1 if point is None else point for point in points) / len(points)
    return max(0.0, lower_mean - radius), min(1.0, upper_mean + radius)
