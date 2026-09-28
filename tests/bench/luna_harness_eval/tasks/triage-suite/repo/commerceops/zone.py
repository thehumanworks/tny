def zone_for(postcode, prefix_to_zone, default):
    """Choose the most-specific matching postal prefix."""
    for prefix, zone_name in prefix_to_zone.items():
        if postcode.startswith(prefix):
            return zone_name
    return default
