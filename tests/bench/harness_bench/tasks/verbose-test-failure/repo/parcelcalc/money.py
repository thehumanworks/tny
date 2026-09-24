def parse_cents(amount):
    # Signed decimal string, at most two fractional digits.
    if not isinstance(amount, str):
        raise ValueError("amount")
    return int(float(amount) * 100)
