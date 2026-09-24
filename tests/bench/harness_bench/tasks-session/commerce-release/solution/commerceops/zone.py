def zone_for(postcode, prefix_to_zone, default):
    """Choose the most-specific matching postal prefix."""
    matches = [
        (len(prefix), zone_name)
        for prefix, zone_name in prefix_to_zone.items()
        if postcode.startswith(prefix)
    ]
    return max(matches)[1] if matches else default
