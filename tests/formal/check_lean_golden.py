#!/usr/bin/env python3
"""Build Lean proofs and compare exported fixtures without editing the checkout."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


def files(directory: Path) -> dict[Path, bytes]:
    result = {}
    for path in directory.rglob("*"):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ValueError(f"golden entry is not a regular file/directory: {path}")
        if path.is_file():
            result[path.relative_to(directory)] = path.read_bytes()
    return result


def check(project: Path, lake: str) -> None:
    expected = files(project / "golden")
    if not expected:
        raise ValueError(f"no committed golden files in {project / 'golden'}")
    # Preserve the existing full-package proof build, including targets the
    # exporter might not import in a future package revision.
    subprocess.run([lake, "build"], cwd=project, check=True, timeout=600)
    with tempfile.TemporaryDirectory(prefix="tny-lean-golden-") as temporary:
        output = Path(temporary) / "golden"
        subprocess.run(
            [lake, "exe", "export", str(output)], cwd=project, check=True, timeout=600
        )
        actual = files(output)
    if files(project / "golden") != expected:
        raise ValueError(
            f"committed golden files changed during verification: {project}"
        )
    differences = {
        "missing": expected.keys() - actual.keys(),
        "extra": actual.keys() - expected.keys(),
        "changed": {
            path
            for path in expected.keys() & actual.keys()
            if expected[path] != actual[path]
        },
    }
    if any(differences.values()):
        details = "\n".join(
            f"{kind}: {', '.join(str(path) for path in sorted(paths))}"
            for kind, paths in differences.items()
            if paths
        )
        raise ValueError(f"golden fixtures differ in {project}:\n{details}")
    print(f"{project}: {len(expected)} golden files match; checkout unchanged")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("--lake", default="lake")
    args = parser.parse_args()
    try:
        check(args.project.resolve(), args.lake)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
