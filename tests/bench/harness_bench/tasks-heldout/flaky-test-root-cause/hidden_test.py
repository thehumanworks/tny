import subprocess
import sys
from pathlib import Path

w = Path(sys.argv[1])
sys.path.insert(0, str(w))
from replay import replay

for n in (0, 1, 37, 129):
    events = [{"seq": i % 4, "message": f"  Item {i}  "} for i in range(n)]
    original = repr(events)
    expected = [{"seq": i % 4, "message": f"item {i}"} for i in range(n)]
    assert replay(events) == expected
    assert repr(events) == original
subprocess.run(
    [sys.executable, "-m", "unittest", "discover", "-s", "tests"],
    cwd=w,
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
