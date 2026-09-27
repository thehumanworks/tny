"""Publish synthetic evidence only; never read authentication or user config."""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import tarfile

from execute import BUILD, ROOT
from run import atomic_json


def main() -> None:
    destination = ROOT / "docs/verification/code-mode-language/data"
    destination.mkdir(parents=True, exist_ok=True)
    files = {
        "samples.json": BUILD / "live/samples.json",
        "experiment.json": BUILD / "live/manifest.json",
        "analysis.json": BUILD / "analysis.json",
        "controls.json": BUILD / "controls.json",
        "native-timings.json": BUILD / "native-timings.json",
        "python-footprint.json": BUILD / "python-footprint.json",
        "boundaries.json": BUILD / "boundaries.json",
        "audit.json": BUILD / "audit.json",
    }
    for name, source in files.items():
        (destination / name).write_bytes(source.read_bytes())
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as archive:
        for source in sorted((BUILD / "live").glob("*/attempt-*/*")):
            if source.name not in {"events.jsonl", "prompt.txt", "generation.json"}:
                continue
            content = source.read_bytes()
            member = tarfile.TarInfo(str(source.relative_to(BUILD / "live")))
            member.size = len(content)
            member.mode = 0o644
            archive.addfile(member, io.BytesIO(content))
    payload = gzip.compress(buffer.getvalue(), mtime=0)
    (destination / "raw-generations.tar.gz").write_bytes(payload)
    manifest = {
        p.name: {
            "bytes": p.stat().st_size,
            "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
        }
        for p in sorted(destination.iterdir())
        if p.is_file() and p.name != "SHA256.json"
    }
    atomic_json(destination / "SHA256.json", manifest)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
