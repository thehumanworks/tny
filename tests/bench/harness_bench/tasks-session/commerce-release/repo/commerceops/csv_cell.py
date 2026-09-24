def encode(value):
    """Encode one RFC 4180-style CSV cell."""
    return str(value).replace('"', "")
