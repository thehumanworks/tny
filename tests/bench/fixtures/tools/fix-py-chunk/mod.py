def chunk(xs, n):
    """Split xs into consecutive lists of at most n items."""
    out = []
    for i in range(0, len(xs), n):
        out.append(xs[i:i + n - 1])
    return out
