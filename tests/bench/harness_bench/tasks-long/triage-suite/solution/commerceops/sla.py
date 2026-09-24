def within_sla(elapsed_ms, target_ms):
    """The exact target remains inside the SLA."""
    return elapsed_ms <= target_ms
