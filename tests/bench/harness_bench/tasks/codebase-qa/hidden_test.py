import json
import sys
from pathlib import Path

w = Path(sys.argv[1])
actual = json.loads((w / "ANSWERS.json").read_text())
expected = dict(
    q1="-2", q2="3", q3="99999999", q4="sizeof(size_t) * 2", q5="consume_trailer"
)
assert all(isinstance(v, str) for v in actual.values())
assert {k: v.strip() for k, v in actual.items()} == expected
assert "Copyright" in (w / "LICENSE").read_text()
