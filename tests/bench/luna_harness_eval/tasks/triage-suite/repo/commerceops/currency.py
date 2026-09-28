def convert_minor(cents, numerator, denominator):
    """Convert integer minor units, rounding half away from zero."""
    return cents // denominator * numerator
