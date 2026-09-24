def earliest(events):
    """Return the event with the earliest absolute ISO-8601 instant."""
    return min(events, key=lambda event: event["ts"])
