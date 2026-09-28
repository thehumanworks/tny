def unique_total(entries):
    """Sum the first occurrence of each transaction ID."""
    seen = set()
    total = 0
    for transaction_id, cents in entries:
        if transaction_id not in seen:
            total += cents
            seen.add(transaction_id)
    return total
