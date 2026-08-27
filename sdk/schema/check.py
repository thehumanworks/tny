#!/usr/bin/env python3
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
schema_path = ROOT / "sdk/schema/events.json"
schema = json.loads(schema_path.read_text())
header = (ROOT / "include/tny/tny.h").read_text()
errors = []
seen = set()
for e in schema["events"]:
    if e["id"] in seen:
        errors.append(f"duplicate event id {e['id']}")
    seen.add(e["id"])
    m = re.search(rf"^#define\s+{re.escape(e['c'])}\s+(\d+)u\s*$", header, re.M)
    if not m:
        errors.append(f"missing public constant {e['c']}")
    elif int(m.group(1)) != e["id"]:
        errors.append(f"{e['c']}={m.group(1)} schema={e['id']}")
# Generated files must be reproducible.
paths = [
    ROOT / "sdk/schema/generated/events.ts",
    ROOT / "sdk/schema/generated/events.py",
    ROOT / "sdk/schema/fixtures/events.json",
]
before = [p.read_bytes() for p in paths]
subprocess.run([sys.executable, str(ROOT / "sdk/schema/generate.py")], check=True)
after = [p.read_bytes() for p in paths]
if before != after:
    errors.append("generated schema artifacts were stale")
if errors:
    print("event schema check failed:", file=sys.stderr)
    for e in errors:
        print(" -", e, file=sys.stderr)
    raise SystemExit(1)
print(f"event schema: {len(schema['events'])} events, generated artifacts current")
