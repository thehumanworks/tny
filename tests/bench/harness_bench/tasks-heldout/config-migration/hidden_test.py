import sys
import tempfile
from pathlib import Path

w = Path(sys.argv[1])
sys.path.insert(0, str(w))
from config import load_config

for index in range(12):
    path = w / "configs" / f"env_{index:02d}.ini"
    assert "cache_dir" not in path.read_text()
    assert load_config(path) == {"state_dir": Path(f"var/cache/{index:02d}")}
for doc in (w / "docs").glob("*.md"):
    assert "cache_dir" not in doc.read_text()
assert len(list((w / "docs").glob("*.md"))) == 8
assert "state_dir" in (w / "README.md").read_text()
with tempfile.TemporaryDirectory() as tmp:
    old = Path(tmp) / "old.ini"
    old.write_text("[storage]\ncache_dir = legacy/path\n")
    assert load_config(old) == {"state_dir": Path("legacy/path")}
    old.write_text("[storage]\ncache_dir = legacy/path\nstate_dir = new/path\n")
    assert load_config(old) == {"state_dir": Path("new/path")}
