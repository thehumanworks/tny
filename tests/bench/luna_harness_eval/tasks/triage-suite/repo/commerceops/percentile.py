def nearest_rank(values, fraction):
    """Return the nearest-rank percentile for 0 < fraction <= 1."""
    ordered = sorted(values)
    return ordered[int(fraction * (len(ordered) - 1))]
