def allowed(granted, required):
    """Check exact comma-separated capability names."""
    return required in {part.strip() for part in granted.split(",")}
