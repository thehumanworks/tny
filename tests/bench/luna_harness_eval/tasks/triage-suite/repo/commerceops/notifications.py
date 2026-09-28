def dedupe(rows):
    """Keep the first notification for each recipient and event."""
    seen = set()
    result = []
    for row in rows:
        if row["event"] not in seen:
            result.append(row)
            seen.add(row["event"])
    return result
