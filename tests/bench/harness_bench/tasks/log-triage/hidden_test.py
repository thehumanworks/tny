import sys
from pathlib import Path

w = Path(sys.argv[1])
assert [
    x.strip() for x in (w / "ANSWER.txt").read_text().splitlines() if x.strip()
] == ["request_id=req-7f3a2c91", "timestamp=2026-04-07T13:42:17.381Z"]
assert (w / "services.log").stat().st_size > 30000000
