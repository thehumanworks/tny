"""Private oracle for incident answers and routing behavior."""

import json
import sys
from pathlib import Path

workspace = Path(sys.argv[1])
sys.path.insert(0, str(workspace))
from gateway import config  # noqa: E402

answers = json.loads((workspace / "ANSWERS.json").read_text())
assert set(answers) == {
    "root_cause",
    "first_failing_request",
    "first_failure_utc",
    "affected_tenants",
    "peak_5m_start_utc",
    "total_503",
}
cause = answers["root_cause"].lower()
assert (
    "default" in cause
    and "premium" in cause
    and ("precedence" in cause or "override" in cause)
)
assert answers["first_failing_request"] == "req-120000"
assert answers["first_failure_utc"] == "2026-06-14T10:20:00Z"
assert answers["affected_tenants"] == 6
assert answers["peak_5m_start_utc"] == "2026-06-14T10:30:00Z"
assert answers["total_503"] == 27000
assert (
    sum(path.stat().st_size for path in (workspace / "evidence").iterdir()) > 90_000_000
)
assert config.pool_capacity_for("tenant-00", "premium") == 80
assert config.pool_capacity_for("tenant-01", "standard") == 20
config.TENANT_POOL_CAPACITY["tenant-00"] = 64
assert config.pool_capacity_for("tenant-00", "premium") == 64
print("incident oracle passed")
