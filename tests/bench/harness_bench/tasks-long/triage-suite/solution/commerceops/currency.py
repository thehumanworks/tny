def convert_minor(cents, numerator, denominator):
    """Convert integer minor units, rounding half away from zero."""
    if denominator <= 0:
        raise ValueError("denominator must be positive")
    sign = -1 if cents < 0 else 1
    magnitude = (2 * abs(cents) * numerator + denominator) // (2 * denominator)
    return sign * magnitude
