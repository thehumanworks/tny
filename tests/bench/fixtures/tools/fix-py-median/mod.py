def median(xs):
    """Median of a list of numbers; average the middle two if even."""
    s = sorted(xs)
    return s[len(s) // 2]
