def next_number(existing):
    """Find the next INV sequence number."""
    return f"INV-{int(sorted(existing)[-1].split('-')[-1]) + 1}"
