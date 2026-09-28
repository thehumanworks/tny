def next_number(existing):
    """Find the next INV sequence number."""
    return f"INV-{max(int(value.split('-')[-1]) for value in existing) + 1}"
