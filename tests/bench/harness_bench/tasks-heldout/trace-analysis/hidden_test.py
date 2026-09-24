import sys
from pathlib import Path

w = Path(sys.argv[1])
trace = w / "trace.ndjson"
assert 45_000_000 <= trace.stat().st_size <= 70_000_000
answer = [
    line.strip() for line in (w / "ANSWER.txt").read_text().splitlines() if line.strip()
]
assert answer == ["endpoint=/v2/search", "failed_count=6538", "exclusive_us=16744251"]
