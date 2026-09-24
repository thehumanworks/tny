import importlib
import json
import sys
from pathlib import Path

w = Path(sys.argv[1])
sys.path.insert(0, str(w))
from core import load_profile, read_profile

path = w / "config.json"
assert list(json.loads(path.read_text())) == ["settings_path"]
assert read_profile(path) == load_profile(path) == "profile=active"
for i in range(16):
    assert (
        importlib.import_module(f"clients.client_{i:02d}").run(path) == "profile=active"
    )
for p in [
    w / "README.md",
    *sorted((w / "docs").glob("*.md")),
    *sorted((w / "clients").glob("*.py")),
]:
    s = p.read_text()
    assert "profile_path" not in s
    if p.parent == w / "clients":
        assert "load_profile" not in s
    else:
        assert "read_profile" in s
assert len(list((w / "docs").glob("*.md"))) == 12
