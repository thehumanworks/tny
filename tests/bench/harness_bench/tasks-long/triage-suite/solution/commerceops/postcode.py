def normalize(value):
    """Uppercase a postal code and remove all whitespace."""
    return "".join(value.upper().split())
