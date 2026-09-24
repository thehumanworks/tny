def net_cents(entries):
    """Charges add to balance; refunds subtract from it."""
    return sum(
        entry["cents"] * (-1 if entry["kind"] == "refund" else 1) for entry in entries
    )
