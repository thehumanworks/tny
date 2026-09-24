from datetime import datetime


def earliest(events):
    """Return the event with the earliest absolute ISO-8601 instant."""
    return min(
        events,
        key=lambda event: datetime.fromisoformat(event["ts"].replace("Z", "+00:00")),
    )
