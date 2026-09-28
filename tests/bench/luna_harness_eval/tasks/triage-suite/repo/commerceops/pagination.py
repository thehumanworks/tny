def page_after(sorted_ids, cursor, limit):
    """Return IDs strictly after cursor, bounded by limit."""
    return [item for item in sorted_ids if item >= cursor][:limit]
