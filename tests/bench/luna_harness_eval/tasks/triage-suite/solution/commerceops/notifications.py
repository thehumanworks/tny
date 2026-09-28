def dedupe(rows):
    """Keep the first notification for each recipient and event."""
    seen = set()
    result = []
    for row in rows:
        key = (row["recipient"], row["event"])
        if key not in seen:
            result.append(row)
            seen.add(key)
    return result
