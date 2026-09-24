"""Pool capacity selection for gateway requests."""

DEFAULT_POOL_CAPACITY = 20
TIER_POOL_CAPACITY = {"premium": 80, "standard": 20}
TENANT_POOL_CAPACITY: dict[str, int] = {}


def pool_capacity_for(tenant: str, tier: str) -> int:
    """Select tenant, then tier, then default capacity."""
    if tenant in TENANT_POOL_CAPACITY:
        return TENANT_POOL_CAPACITY[tenant]
    return TIER_POOL_CAPACITY.get(tier, DEFAULT_POOL_CAPACITY)
