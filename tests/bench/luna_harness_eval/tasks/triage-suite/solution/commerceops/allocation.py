def allocate_cents(total, weights):
    """Allocate all cents by largest remainder, ties by original index."""
    denominator = sum(weights)
    if denominator <= 0:
        raise ValueError("weights must have positive total")
    floors = [total * weight // denominator for weight in weights]
    residual = total - sum(floors)
    order = sorted(
        range(len(weights)),
        key=lambda index: (-(total * weights[index] % denominator), index),
    )
    for index in order[:residual]:
        floors[index] += 1
    return floors
