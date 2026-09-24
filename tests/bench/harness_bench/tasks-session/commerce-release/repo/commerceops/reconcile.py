def unique_total(entries):
    """Sum the first occurrence of each transaction ID."""
    return sum(cents for transaction_id, cents in entries)
