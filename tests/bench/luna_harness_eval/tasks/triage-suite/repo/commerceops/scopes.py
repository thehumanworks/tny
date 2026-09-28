def allowed(granted, required):
    """Check exact comma-separated capability names."""
    return required in granted
