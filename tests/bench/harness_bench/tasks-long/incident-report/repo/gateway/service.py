"""A minimal deterministic gateway model for incident analysis."""

from dataclasses import dataclass

from gateway.config import pool_capacity_for


@dataclass(frozen=True)
class Request:
    request_id: str
    tenant: str
    tier: str
    concurrent_work: int


def route(request: Request) -> tuple[int, str]:
    """Return the status and selected pool name."""
    capacity = pool_capacity_for(request.tenant, request.tier)
    pool = "premium" if request.tier == "premium" else "standard"
    return (503 if request.concurrent_work > capacity else 200, pool)
