def allocate_cents(total, weights):
    """Allocate all cents by largest remainder, ties by original index."""
    return [round(total * weight / sum(weights)) for weight in weights]
