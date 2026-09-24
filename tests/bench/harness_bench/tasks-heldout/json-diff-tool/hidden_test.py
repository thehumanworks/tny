import importlib.util
import json
import subprocess
import sys
from pathlib import Path

w = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("jdiff", w / "jdiff.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.diff_values({"a/b": {"~": [1, 2]}}, {"a/b": {"~": [1, 3, 4]}}) == [
    {"path": "/a~1b/~0/1", "kind": "changed", "before": 2, "after": 3},
    {"path": "/a~1b/~0/2", "kind": "added", "after": 4},
]
assert module.diff_values(True, 1) == [
    {"path": "", "kind": "changed", "before": True, "after": 1}
]
assert module.diff_values({"x": {"a": 1}, "z": 3}, {"x": {"a": 2}, "y": 4}, ["/x"]) == [
    {"path": "/y", "kind": "added", "after": 4},
    {"path": "/z", "kind": "removed", "before": 3},
]
assert module.diff_values([], [1]) == [{"path": "/0", "kind": "added", "after": 1}]


def cli(*args):
    run = subprocess.run(
        [sys.executable, "jdiff.py", "left.json", "right.json", *args],
        cwd=w,
        text=True,
        capture_output=True,
        check=True,
    )
    return json.loads(run.stdout)


assert cli() == [
    {"path": "/items/1", "kind": "changed", "before": 2, "after": 3},
    {"path": "/items/2", "kind": "added", "after": 4},
    {"path": "/new", "kind": "added", "after": True},
    {"path": "/old", "kind": "removed", "before": True},
]
assert cli("--ignore", "/items", "--summary") == {
    "added": 1,
    "removed": 1,
    "changed": 0,
}
assert (
    "--summary"
    in subprocess.run(
        [sys.executable, "jdiff.py", "--help"],
        cwd=w,
        text=True,
        capture_output=True,
        check=True,
    ).stdout
)
subprocess.run(
    [sys.executable, "test_public.py"], cwd=w, check=True, stdout=subprocess.DEVNULL
)
