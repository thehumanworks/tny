#!/usr/bin/env python3
"""Plan and verify idempotent PyPI publication using JSON and artifact hashes."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Callable


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def expected_wheels(root: Path, version: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for path in sorted(root.rglob("tny-*.whl")):
        with zipfile.ZipFile(path) as archive:
            metadata_names = [
                name
                for name in archive.namelist()
                if name.endswith(".dist-info/METADATA")
            ]
            if len(metadata_names) != 1:
                raise ValueError(f"{path}: wheel has no unique METADATA")
            metadata = archive.read(metadata_names[0]).decode("utf-8")
        if (
            "\nName: tny\n" not in f"\n{metadata}"
            or f"\nVersion: {version}\n" not in f"\n{metadata}"
        ):
            raise ValueError(f"{path}: wheel metadata does not match tny {version}")
        value = {"path": path, "sha256": sha256(path)}
        prior = result.get(path.name)
        if prior:
            relation = (
                "non-identical" if prior["sha256"] != value["sha256"] else "duplicate"
            )
            raise ValueError(f"{relation} wheel {path.name}")
        result[path.name] = value
    if len(result) != 3:
        raise ValueError(
            f"expected exactly three platform wheels, got {sorted(result)}"
        )
    return result


def assess_release(
    release: dict[str, Any] | None, expected: dict[str, dict[str, Any]]
) -> bool:
    """Return whether publication is needed; reject partial or mismatched reruns."""
    if release is None:
        return True
    observed = {
        item["filename"]: item
        for item in release.get("urls", [])
        if item.get("packagetype") == "bdist_wheel"
    }
    if set(observed) != set(expected):
        raise ValueError(
            f"PyPI release is partial or has unexpected wheels: expected {sorted(expected)}, "
            f"got {sorted(observed)}"
        )
    for filename, local in expected.items():
        remote = observed[filename]
        if remote.get("digests", {}).get("sha256") != local["sha256"]:
            raise ValueError(f"PyPI digest mismatch for {filename}")
    return False


def fetch_release(base_url: str, version: str) -> dict[str, Any] | None:
    url = f"{base_url.rstrip('/')}/pypi/tny/{version}/json"
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            if response.status != 200:
                raise ValueError(f"PyPI JSON readback failed: HTTP {response.status}")
            return json.load(response)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise ValueError(f"PyPI JSON readback failed: HTTP {error.code}") from error


def download_and_verify(
    release: dict[str, Any],
    expected: dict[str, dict[str, Any]],
    destination: Path,
    fetch_bytes: Callable[[str], bytes] | None = None,
) -> None:
    destination.mkdir(parents=True, exist_ok=True)

    def default_fetch(url: str) -> bytes:
        with urllib.request.urlopen(url, timeout=60) as response:
            return response.read()

    fetch = fetch_bytes or default_fetch
    urls = {
        item["filename"]: item
        for item in release.get("urls", [])
        if item.get("packagetype") == "bdist_wheel"
    }
    for filename in sorted(expected):
        item = urls[filename]
        data = fetch(item["url"])
        actual = hashlib.sha256(data).hexdigest()
        if actual != expected[filename]["sha256"] or actual != item.get(
            "digests", {}
        ).get("sha256"):
            raise ValueError(f"downloaded PyPI hash mismatch for {filename}")
        (destination / filename).write_bytes(data)


def write_output(path: Path | None, name: str, value: str) -> None:
    if path is None:
        return
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"{name}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("plan", "readback"), required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--base-url", default="https://pypi.org")
    parser.add_argument("--github-output", type=Path)
    parser.add_argument("--download-dir", type=Path)
    parser.add_argument("--attempts", type=int, default=10)
    args = parser.parse_args()
    if args.attempts < 1:
        raise SystemExit("--attempts must be positive")
    expected = expected_wheels(args.root, args.version)
    if args.mode == "plan":
        release = fetch_release(args.base_url, args.version)
        required = assess_release(release, expected)
        write_output(args.github_output, "publish_required", str(required).lower())
        print(
            json.dumps(
                {"version": args.version, "publish_required": required}, sort_keys=True
            )
        )
        return 0
    release = None
    for attempt in range(1, args.attempts + 1):
        release = fetch_release(args.base_url, args.version)
        if release is not None:
            try:
                assess_release(release, expected)
                break
            except ValueError:
                if attempt == args.attempts:
                    raise
        if attempt < args.attempts:
            time.sleep(1)
    if release is None:
        raise ValueError(f"PyPI release tny {args.version} did not appear")
    assess_release(release, expected)
    if args.download_dir is None:
        raise ValueError("--download-dir is required for readback")
    download_and_verify(release, expected, args.download_dir)
    print(json.dumps({"version": args.version, "readback": "verified"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
