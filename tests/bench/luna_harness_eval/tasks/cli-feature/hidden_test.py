import json
import subprocess
import sys
from pathlib import Path

w = Path(sys.argv[1])


def run(*args):
    return subprocess.run(
        [sys.executable, "journal.py", *args], cwd=w, text=True, capture_output=True
    )


for flags, expected in [
    ((), (3, 90)),
    (("--since", "2026-03-04"), (2, 70)),
    (("--tag", "ops"), (2, 50)),
    (("--since", "2026-03-04", "--tag", "ops"), (1, 30)),
    (("--tag", "missing"), (0, 0)),
]:
    result = run("stats", *flags)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == dict(
        count=expected[0], total_minutes=expected[1]
    )
assert run("stats", "--since", "nope").returncode != 0
assert (
    "--since" in run("stats", "--help").stdout
    and "--tag" in run("stats", "--help").stdout
)
readme = (w / "README.md").read_text()
assert all(
    word in readme for word in ("stats", "--since", "--tag", "count", "total_minutes")
)
