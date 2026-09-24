def endpoint_for(tenant, overrides, default):
    """An explicit tenant override, including an empty disable value, wins."""
    return overrides[tenant] if tenant in overrides else default
