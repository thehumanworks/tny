import unittest

from gateway.config import DEFAULT_POOL_CAPACITY, pool_capacity_for
from gateway.service import Request, route


class PoolRoutingTests(unittest.TestCase):
    def test_standard_uses_default(self):
        self.assertEqual(
            pool_capacity_for("tenant-01", "standard"), DEFAULT_POOL_CAPACITY
        )

    def test_premium_uses_tier_override(self):
        self.assertEqual(pool_capacity_for("tenant-00", "premium"), 80)

    def test_premium_burst_stays_available(self):
        request = Request("example", "tenant-00", "premium", 50)
        self.assertEqual(route(request), (200, "premium"))
