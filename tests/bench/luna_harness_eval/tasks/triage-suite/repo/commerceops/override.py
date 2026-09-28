def endpoint_for(tenant, overrides, default):
    """An explicit tenant override, including an empty disable value, wins."""
    return overrides.get(tenant) or default
