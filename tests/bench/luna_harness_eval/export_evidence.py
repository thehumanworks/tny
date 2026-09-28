"""Export synthetic receipts and final source overlays, excluding credentials/state.

The export removes opaque encrypted reasoning handles and unused legacy price
estimates. Original wire-body hashes remain in the receipts; per-file export
hashes explicitly distinguish the sanitized representation from the raw bytes.
"""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import re
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def clean(value):
    if isinstance(value, dict):
        return {
            key: clean(item)
            for key, item in value.items()
            if key not in {"encrypted_content", "usd", "ite"}
        }
    if isinstance(value, list):
        return [clean(item) for item in value]
    return value


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    state = json.loads((HERE / "STATE.json").read_text())
    root = Path(state["temporary_root"])
    target = HERE.parents[2] / "docs/benchmarks/tny-codex-luna-20260928/data"
    target.mkdir(parents=True, exist_ok=True)
    if (root / "primary.exit").read_text().strip() != "0" or (
        root / "cache.exit"
    ).read_text().strip() != "0":
        raise ValueError("Incomplete inference run")
    members = {}
    records = []
    omitted = []

    def add(name, data, original=None):
        if name in members:
            raise ValueError("Duplicate archive path " + name)
        members[name] = data
        records.append(
            {
                "path": name,
                "bytes": len(data),
                "sha256": digest(data),
                "original_sha256": digest(original if original is not None else data),
                "sanitized": original is not None and data != original,
            }
        )

    def file(path, name):
        raw = path.read_bytes()
        if path.name.endswith(".json.gz"):
            plain = gzip.decompress(raw)
            content = (
                json.dumps(
                    clean(json.loads(plain)), ensure_ascii=False, separators=(",", ":")
                ).encode()
                + b"\n"
            )
            add(name.removesuffix(".gz"), content, plain)
        elif path.suffix == ".json":
            content = (
                json.dumps(clean(json.loads(raw)), ensure_ascii=False, indent=2) + "\n"
            ).encode()
            add(name, content, raw)
        elif path.suffix == ".jsonl":
            content = (
                "\n".join(
                    json.dumps(clean(json.loads(line)), ensure_ascii=False)
                    for line in raw.decode().splitlines()
                    if line.strip()
                )
                + "\n"
            ).encode()
            add(name, content, raw)
        else:
            add(name, raw)

    aggregates = [
        "primary-manifest.json",
        "smoke-manifest.json",
        "smoke-results.json",
        "analysis.json",
        "cache-results.json",
        "cache-analysis.json",
        "call-diagnostics.json",
        "quality-review.json",
        "controls-final.log",
        "primary.log",
        "primary.exit",
        "cache.log",
        "cache.exit",
    ]
    for name in aggregates:
        file(root / name, "summary/" + name)
    allowed = {
        "result.json",
        "initial-files.json",
        "task.json",
        "commands.json",
        "agent.patch",
        "integrity.json",
        "verify.log",
        "final_message.txt",
        "stdout.txt",
        "stderr.txt",
        "turns.json",
    }
    for folder in (root / "output").glob("*"):
        for p in folder.rglob("*"):
            if not p.is_file() or any(
                s in p.parts for s in ["home", "workspace", ".git"]
            ):
                continue
            if (
                p.name in allowed
                or p.parent.name == "proxy"
                or re.match(r"turn-\d+\.(stdout|stderr)\.txt$", p.name)
            ):
                file(p, "runs/" + str(p.relative_to(root / "output")))
    for d in (root / "output/primary").glob("*/*/rep-*"):
        before = json.loads((d / "initial-files.json").read_text())
        workspace = d / "workspace"
        prefix = "runs/primary/" + str(d.relative_to(root / "output/primary"))
        changed = []
        for p in workspace.rglob("*"):
            if not p.is_file() or any(
                s in p.relative_to(workspace).parts for s in (".git", "__pycache__")
            ):
                continue
            name = str(p.relative_to(workspace))
            raw = p.read_bytes()
            if name in before and digest(raw) == before[name]:
                continue
            if len(raw) > 1024 * 1024 or p.suffix in (
                ".pyc",
                ".so",
                ".o",
                ".db",
                ".sqlite",
            ):
                omitted.append(
                    {
                        "run": prefix,
                        "file": name,
                        "bytes": len(raw),
                        "reason": "generated binary/large output",
                    }
                )
                continue
            try:
                raw.decode("utf-8")
            except UnicodeError:
                omitted.append(
                    {
                        "run": prefix,
                        "file": name,
                        "bytes": len(raw),
                        "reason": "non-text generated artifact",
                    }
                )
                continue
            add(prefix + "/final-source/" + name, raw)
            changed.append(name)
        deleted = [n for n in before if not (workspace / n).exists()]
        add(
            prefix + "/source-overlay.json",
            (
                json.dumps({"changed_or_new": changed, "deleted": deleted}, indent=2)
                + "\n"
            ).encode(),
        )
    pattern = re.compile(
        rb'(?<![A-Za-z0-9_-])(?:sk-(?:proj-)?[A-Za-z0-9_-]{25,}|Bearer\s+[A-Za-z0-9._-]{30,}|"access_token"\s*:\s*"[^"\s]{20,})'
    )
    for name, data in members.items():
        if pattern.search(data):
            raise ValueError("Potential credential detected in " + name)
    inventory = {
        "files": records,
        "omitted_generated_files": omitted,
        "scope": "Only synthetic benchmark inputs/outputs. No HOME, auth/config stores, binaries, Git internals or third-party session databases. Opaque encrypted_content fields and unused legacy dollar/token-equivalent estimates removed. Original provider usage and original SHA hashes are retained; sanitized wire JSON has a separate hash.",
    }
    add("INVENTORY.json", (json.dumps(inventory, indent=2) + "\n").encode())
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as archive:
        for name, data in sorted(members.items()):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
    payload = gzip.compress(buffer.getvalue(), mtime=0)
    (target / "evidence.tar.gz").write_bytes(payload)
    for name in [
        "analysis.json",
        "cache-analysis.json",
        "call-diagnostics.json",
        "quality-review.json",
        "primary-manifest.json",
    ]:
        content = clean(json.loads((root / name).read_text()))
        if name == "call-diagnostics.json":
            content.pop("runs", None)
        (target / name).write_text(json.dumps(content, indent=2) + "\n")
    manifest = {
        p.name: {"bytes": p.stat().st_size, "sha256": digest(p.read_bytes())}
        for p in target.iterdir()
        if p.is_file() and p.name != "SHA256.json"
    }
    (target / "SHA256.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(
        json.dumps(
            {
                "archive_bytes": len(payload),
                "members": len(members),
                "sha256": digest(payload),
                "path": str(target),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
