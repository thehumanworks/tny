#!/usr/bin/env python3
"""Synthetic cross-platform aggregation and negative release fixtures."""
from __future__ import annotations

import importlib.util
import hashlib
import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "validate_sdk_release.py"
spec = importlib.util.spec_from_file_location("validate_sdk_release", SCRIPT)
assert spec is not None and spec.loader is not None
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class ReleaseValidatorTests(unittest.TestCase):
    def test_synthetic_three_platform_cardinality_passes(self) -> None:
        counts = {"@thehumanworks/tny": 3, **{name: 1 for name in validator.NATIVE_PACKAGES}}
        validator.validate_artifact_cardinality(
            counts, ["darwin-arm64", "linux-aarch64", "linux-x86_64"], False
        )

    def test_complete_synthetic_three_platform_aggregation_passes(self) -> None:
        version = "1.2.3"
        commit = "a" * 40

        def archive_bytes(files: dict[str, bytes]) -> bytes:
            output = io.BytesIO()
            with tarfile.open(fileobj=output, mode="w:gz") as archive:
                for name, data in sorted(files.items()):
                    info = tarfile.TarInfo(f"package/{name}")
                    info.size = len(data)
                    info.mtime = 0
                    archive.addfile(info, io.BytesIO(data))
            return output.getvalue()

        optional = {name: version for name in validator.NATIVE_PACKAGES}
        meta_files = {
            "package.json": json.dumps({
                "name": "@thehumanworks/tny", "version": version,
                "optionalDependencies": optional,
            }).encode(),
            "dist/index.mjs": b"export {};\n",
            "dist/index.d.ts": b"export {};\n",
            "scripts/native-loader.mjs": b"export {};\n",
        }
        meta_data = archive_bytes(meta_files)
        triples = {
            "@thehumanworks/tny-darwin-arm64": ("darwin", "arm64", "libtny.0.dylib", "tny-1.2.3-py3-none-macosx_13_0_arm64.whl"),
            "@thehumanworks/tny-linux-arm64": ("linux", "arm64", "libtny.so.0", "tny-1.2.3-py3-none-manylinux_2_34_aarch64.whl"),
            "@thehumanworks/tny-linux-x64": ("linux", "x64", "libtny.so.0", "tny-1.2.3-py3-none-manylinux_2_34_x86_64.whl"),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, (package_name, (os_name, architecture, library, wheel_name)) in enumerate(triples.items()):
                lane = root / str(index)
                lane.mkdir()
                meta_name = "thehumanworks-tny-1.2.3.tgz"
                (lane / meta_name).write_bytes(meta_data)
                addon = f"addon-{package_name}".encode()
                library_data = f"library-{package_name}".encode()
                manifest = {
                    "schemaVersion": 2, "packageName": package_name,
                    "packageVersion": version, "releaseTag": "v1.2.3",
                    "platform": os_name, "architecture": architecture,
                    "abiMajor": 0, "abiMinor": 7, "libraryVersion": version,
                    "capabilityStructSize": 344, "linkage": "shared-loader-relative",
                    "addonSha256": hashlib.sha256(addon).hexdigest(),
                    "librarySha256": hashlib.sha256(library_data).hexdigest(),
                    **({"minimumOs": "13.0"} if os_name == "darwin" else {"minimumGlibc": "2.34"}),
                }
                native_package = {
                    "name": package_name, "version": version,
                    "os": [os_name], "cpu": [architecture],
                    **({"libc": ["glibc"]} if os_name == "linux" else {}),
                }
                native_files = {
                    "package.json": json.dumps(native_package).encode(),
                    "prebuilds/manifest.json": json.dumps(manifest).encode(),
                    "prebuilds/tny.node": addon,
                    f"prebuilds/{library}": library_data,
                }
                native_data = archive_bytes(native_files)
                native_name = f"thehumanworks-{package_name.split('/')[-1]}-1.2.3.tgz"
                (lane / native_name).write_bytes(native_data)
                wheel_buffer = io.BytesIO()
                with zipfile.ZipFile(wheel_buffer, "w") as wheel:
                    wheel.writestr("tny/__init__.py", "")
                    wheel.writestr("tny-1.2.3.dist-info/METADATA", "Name: tny\nVersion: 1.2.3\n")
                wheel_data = wheel_buffer.getvalue()
                (lane / wheel_name).write_bytes(wheel_data)
                digest = lambda data: hashlib.sha256(data).hexdigest()
                native_artifacts = [
                    {"name": "tny.node", "packagePath": "package/prebuilds/tny.node", "sha256": digest(addon)},
                    {"name": library, "packagePath": f"package/prebuilds/{library}", "sha256": digest(library_data)},
                ]
                wheel_artifact = {"name": wheel_name, "sha256": digest(wheel_data)}
                provenance = {
                    "schemaVersion": 1, "releaseTag": "v1.2.3", "npmVersion": version,
                    "pythonVersion": version, "commitSha": commit,
                    "buildTools": {"node": "v24.8.0", "npm": "11.6.0"},
                    "target": {"packageName": package_name},
                    "packages": [
                        {"name": meta_name, "sha256": digest(meta_data), "role": "npm-meta-package"},
                        {"name": native_name, "sha256": digest(native_data), "role": "npm-native-package"},
                    ],
                    "nativeArtifacts": native_artifacts,
                    "pythonWheel": wheel_artifact,
                    "contents": {
                        "meta": sorted(meta_files), "native": sorted(native_files),
                        "wheel": ["tny-1.2.3.dist-info/METADATA", "tny/__init__.py"],
                    },
                }
                (lane / f"{index}.provenance.json").write_text(json.dumps(provenance), encoding="utf-8")
                spdx = {
                    "spdxVersion": "SPDX-2.3", "comment": f"source commit {commit}",
                    "packages": [{
                        "name": package_name, "versionInfo": version,
                        "checksums": [{"algorithm": "SHA256", "checksumValue": digest(native_data)}],
                    }],
                    "files": [
                        {"fileName": item["name"], "checksums": [{"algorithm": "SHA256", "checksumValue": item["sha256"]}]}
                        for item in [*native_artifacts, wheel_artifact]
                    ],
                }
                (lane / f"{index}.spdx.json").write_text(json.dumps(spdx), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, str(SCRIPT), "--tag", "v1.2.3", "--root", str(root),
                 "--commit", commit],
                text=True, capture_output=True, check=False,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_duplicate_native_and_wheel_fail(self) -> None:
        counts = {"@thehumanworks/tny": 3, **{name: 1 for name in validator.NATIVE_PACKAGES}}
        counts["@thehumanworks/tny-linux-x64"] = 2
        with self.assertRaisesRegex(SystemExit, "native npm package cardinality"):
            validator.validate_artifact_cardinality(
                counts, ["darwin-arm64", "linux-aarch64", "linux-x86_64"], False
            )
        counts["@thehumanworks/tny-linux-x64"] = 1
        with self.assertRaisesRegex(SystemExit, "wheel target cardinality"):
            validator.validate_artifact_cardinality(
                counts,
                ["darwin-arm64", "linux-aarch64", "linux-x86_64", "linux-x86_64"],
                False,
            )

    def test_meta_package_missing_loader_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "meta.tgz"
            package = {
                "name": "@thehumanworks/tny",
                "version": "1.2.3",
                "optionalDependencies": {
                    name: "1.2.3" for name in validator.NATIVE_PACKAGES
                },
            }
            with tarfile.open(path, "w:gz") as archive:
                for name, data in {
                    "package/package.json": json.dumps(package).encode(),
                    "package/dist/index.mjs": b"",
                    "package/dist/index.d.ts": b"",
                }.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
            with self.assertRaisesRegex(SystemExit, "meta package is incomplete"):
                validator.validate_npm(path, "v1.2.3", "1.2.3")

    def test_bad_provenance_and_sbom_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bad.provenance.json").write_text(
                json.dumps({
                    "schemaVersion": 1,
                    "releaseTag": "v1.2.3",
                    "npmVersion": "1.2.3",
                    "pythonVersion": "1.2.3",
                    "commitSha": "bad",
                    "target": {"packageName": "@thehumanworks/tny-linux-x64"},
                }), encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, "commit SHA"):
                validator.validate_release_inputs(root, "v1.2.3", "1.2.3", "1.2.3")
            (root / "bad.provenance.json").unlink()
            (root / "bad.spdx.json").write_text(
                json.dumps({"spdxVersion": "SPDX-2.2", "packages": []}), encoding="utf-8"
            )
            with self.assertRaisesRegex(SystemExit, "SPDX document structure"):
                validator.validate_release_inputs(root, "v1.2.3", "1.2.3", "1.2.3")

    def test_bad_wheel_platform_and_metadata_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tny-1.2.3-py3-none-macosx_14_0_arm64.whl"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("tny-1.2.3.dist-info/METADATA", "Name: tny\nVersion: 1.2.3\n")
            with self.assertRaisesRegex(SystemExit, "macOS wheel minimum exceeds 13.0"):
                validator.validate_wheel(path, "1.2.3")


if __name__ == "__main__":
    unittest.main()
