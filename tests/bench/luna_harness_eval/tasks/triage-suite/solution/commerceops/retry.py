def should_retry(code, attempt, max_attempts):
    """Retry transient errors while attempts remain."""
    return code in {"timeout", "unavailable", "rate_limited"} and attempt < max_attempts
