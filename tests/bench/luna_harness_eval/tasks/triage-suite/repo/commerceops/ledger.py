def net_cents(entries):
    """Charges add to balance; refunds subtract from it."""
    return sum(entry["cents"] for entry in entries)
