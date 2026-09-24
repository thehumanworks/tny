def should_retry(code, attempt, max_attempts):
    """Retry transient errors while attempts remain."""
    return code != "ok" and attempt <= max_attempts
