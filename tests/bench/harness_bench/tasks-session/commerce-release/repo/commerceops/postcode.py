def normalize(value):
    """Uppercase a postal code and remove all whitespace."""
    return value.strip().upper().replace(" ", "", 1)
