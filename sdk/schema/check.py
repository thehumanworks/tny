#!/usr/bin/env python3
import json
import re
import subprocess
import sys
import tempfile
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
# Generate in isolation: checking drift must never repair or modify the input.
paths = [
    Path("generated/events.ts"),
    Path("generated/events.py"),
    Path("fixtures/events.json"),
]
with tempfile.TemporaryDirectory(prefix="tny-event-schema-") as directory:
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "sdk/schema/generate.py"),
            "--output-dir",
            directory,
        ],
        check=True,
    )
    for path in paths:
        committed = ROOT / "sdk/schema" / path
        if (
            not committed.is_file()
            or committed.read_bytes() != (Path(directory) / path).read_bytes()
        ):
            errors.append(f"generated schema artifact is missing or stale: {path}")
if errors:
    print("event schema check failed:", file=sys.stderr)
    for e in errors:
        print(" -", e, file=sys.stderr)
    raise SystemExit(1)
print(f"event schema: {len(schema['events'])} events, generated artifacts current")
