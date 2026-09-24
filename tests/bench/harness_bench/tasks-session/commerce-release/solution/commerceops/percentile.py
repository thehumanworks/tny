import math


def nearest_rank(values, fraction):
    """Return the nearest-rank percentile for 0 < fraction <= 1."""
    ordered = sorted(values)
    return ordered[math.ceil(fraction * len(ordered)) - 1]
