#!/usr/bin/env python3
"""Offline validation for tag-derived Python and npm SDK release artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tarfile
import zipfile
from pathlib import Path
from typing import Any

TAG = re.compile(
    r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-(a|b|rc)\.(0|[1-9]\d*))?$"
)
NATIVE_PACKAGES = {
    "@thehumanworks/tny-darwin-arm64": ("darwin", "arm64", "libtny.1.dylib"),
    "@thehumanworks/tny-linux-x64": ("linux", "x64", "libtny.so.1"),
    "@thehumanworks/tny-linux-arm64": ("linux", "arm64", "libtny.so.1"),
}


def versions(tag: str) -> tuple[str, str]:
    match = TAG.fullmatch(tag)
    if match is None:
        raise SystemExit(f"invalid release tag: {tag!r}")
    base = ".".join(match.group(index) for index in range(1, 4))
    if match.group(4) is None:
        return base, base
    return (
        f"{base}-{match.group(4)}.{match.group(5)}",
        f"{base}{match.group(4)}{match.group(5)}",
    )


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tar_member(archive: tarfile.TarFile, name: str) -> bytes:
    member = archive.getmember(name)
    if not member.isfile() or member.size > 64 * 1024 * 1024:
        raise SystemExit(f"unsafe or oversized npm package member: {name}")
    stream = archive.extractfile(member)
    if stream is None:
        raise SystemExit(f"npm package member is unreadable: {name}")
    return stream.read()


def validate_npm(
    path: Path, tag: str, npm_version: str, expected_license: bytes | None = None
) -> tuple[str, str]:
    with tarfile.open(path, "r:gz") as archive:
        names = {member.name for member in archive.getmembers()}
        if any(name.endswith(("libtny.so.0", "libtny.0.dylib")) for name in names):
            raise SystemExit(f"{path}: ABI0 library leaked into an ABI1 npm package")
        package = json.loads(tar_member(archive, "package/package.json"))
        name = package.get("name")
        if package.get("version") != npm_version:
            raise SystemExit(f"{path}: npm version does not match the tag")
        if expected_license is not None:
            if (
                "package/LICENSE" not in names
                or tar_member(archive, "package/LICENSE") != expected_license
            ):
                raise SystemExit(
                    f"{path}: packaged license does not match the repository license"
                )
            if package.get("license") == "UNLICENSED":
                raise SystemExit(f"{path}: npm license metadata remains unpublishable")
        if name == "@thehumanworks/tny":
            expected = {candidate: npm_version for candidate in NATIVE_PACKAGES}
            if package.get("optionalDependencies") != expected:
                raise SystemExit(
                    f"{path}: meta package optional dependencies are not exact and complete"
                )
            if package.get("dependencies") or package.get("scripts"):
                raise SystemExit(
                    f"{path}: meta package must have no runtime dependencies or lifecycle scripts"
                )
            forbidden = [
                member
                for member in names
                if member.startswith("package/build/")
                or member.startswith("package/native/")
            ]
            if forbidden:
                raise SystemExit(
                    f"{path}: meta package leaks native build inputs: {forbidden}"
                )
            required = {
                "package/dist/index.mjs",
                "package/dist/index.d.ts",
                "package/scripts/native-loader.mjs",
            }
            if not required.issubset(names):
                raise SystemExit(f"{path}: meta package is incomplete")
            return name, digest(path.read_bytes())
        if name not in NATIVE_PACKAGES:
            raise SystemExit(f"{path}: unexpected npm package {name!r}")
        if (
            package.get("dependencies")
            or package.get("optionalDependencies")
            or package.get("scripts")
        ):
            raise SystemExit(
                f"{path}: native package must not execute or install JavaScript dependencies"
            )
        os_name, architecture, library = NATIVE_PACKAGES[name]
        manifest = json.loads(tar_member(archive, "package/prebuilds/manifest.json"))
        addon_data = tar_member(archive, "package/prebuilds/tny.node")
        library_data = tar_member(archive, f"package/prebuilds/{library}")
        expected_manifest: dict[str, Any] = {
            "schemaVersion": 3,
            "packageName": name,
            "packageVersion": npm_version,
            "platform": os_name,
            "architecture": architecture,
            "releaseTag": tag,
            "libraryVersion": npm_version,
            "abiMajor": 1,
        }
        if any(manifest.get(key) != value for key, value in expected_manifest.items()):
            raise SystemExit(
                f"{path}: native package manifest identity is inconsistent"
            )
        expected_identity = (
            {"kind": "install_name", "value": "@rpath/libtny.1.dylib"}
            if os_name == "darwin"
            else {"kind": "soname", "value": "libtny.so.1"}
        )
        if manifest.get("dynamicIdentity") != expected_identity:
            raise SystemExit(f"{path}: native package dynamic identity is inconsistent")
        if os_name == "darwin":
            minimum = manifest.get("minimumOs")
            try:
                minimum_tuple = tuple(int(part) for part in minimum.split("."))
            except (AttributeError, ValueError):
                raise SystemExit(f"{path}: macOS minimum version is invalid") from None
            if minimum_tuple > (13, 0):
                raise SystemExit(f"{path}: macOS minimum {minimum} exceeds 13.0")
        if manifest.get("addonSha256") != digest(addon_data) or manifest.get(
            "librarySha256"
        ) != digest(library_data):
            raise SystemExit(f"{path}: native payload hash mismatch")
        if package.get("os") != [os_name] or package.get("cpu") != [architecture]:
            raise SystemExit(f"{path}: npm platform constraints are inconsistent")
        if os_name == "linux" and package.get("libc") != ["glibc"]:
            raise SystemExit(f"{path}: Linux package must be constrained to glibc")
        return name, digest(path.read_bytes())


def validate_wheel(
    path: Path, python_version: str, expected_license: bytes | None = None
) -> str:
    with zipfile.ZipFile(path) as archive:
        if any(
            name.endswith(("libtny.so.0", "libtny.0.dylib"))
            for name in archive.namelist()
        ):
            raise SystemExit(f"{path}: ABI0 library leaked into an ABI1 wheel")
        metadata_names = [
            name for name in archive.namelist() if name.endswith(".dist-info/METADATA")
        ]
        if len(metadata_names) != 1:
            raise SystemExit(f"{path}: wheel has no unique METADATA")
        metadata = archive.read(metadata_names[0]).decode("utf-8")
        if (
            "\nName: tny\n" not in f"\n{metadata}"
            or f"\nVersion: {python_version}\n" not in f"\n{metadata}"
        ):
            raise SystemExit(
                f"{path}: Python package name/version does not match the tag"
            )
        if expected_license is not None:
            license_names = [
                name for name in archive.namelist() if name == "tny/LICENSE"
            ]
            if (
                len(license_names) != 1
                or archive.read(license_names[0]) != expected_license
            ):
                raise SystemExit(
                    f"{path}: packaged license does not match the repository license"
                )
            if "LicenseRef-UNLICENSED" in metadata:
                raise SystemExit(
                    f"{path}: wheel license metadata remains unpublishable"
                )
    filename = path.name
    mac_match = re.search(r"macosx_(\d+)_(\d+)_arm64\.whl$", filename)
    if mac_match:
        if (int(mac_match.group(1)), int(mac_match.group(2))) > (13, 0):
            raise SystemExit(f"{path}: macOS wheel minimum exceeds 13.0")
        return "darwin-arm64"
    if "manylinux" in filename and filename.endswith("_x86_64.whl"):
        return "linux-x86_64"
    if "manylinux" in filename and filename.endswith("_aarch64.whl"):
        return "linux-aarch64"
    raise SystemExit(f"{path}: unexpected Python wheel platform tag")


def require_publishable_license(repo: Path) -> None:
    if not any(
        (repo / name).is_file() for name in ("LICENSE", "LICENSE.txt", "LICENSE.md")
    ):
        raise SystemExit(
            "registry publication is disabled: repository owner license is absent"
        )
    npm = json.loads((repo / "sdk/typescript/package.json").read_text())
    pyproject = (repo / "sdk/python/pyproject.toml").read_text()
    if npm.get("license") == "UNLICENSED" or "LicenseRef-UNLICENSED" in pyproject:
        raise SystemExit(
            "registry publication is disabled: SDK metadata remains unlicensed"
        )


def require_active_abi_baseline(repo: Path) -> None:
    path = repo / "abi/baseline-v1.json"
    try:
        baseline = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(
            f"release publication is disabled: ABI1 baseline is unreadable: {error}"
        ) from None
    if not isinstance(baseline, dict):
        raise SystemExit(
            "release publication is disabled: ABI1 baseline identity is invalid"
        )
    abi = baseline.get("abi")
    expected_abi = {
        "major": 1,
        "minor": 0,
        "encoded": 65536,
        "elf_version_node": "LIBTNY_1.0",
    }
    if baseline.get("schema_version") != 1 or abi != expected_abi:
        raise SystemExit(
            "release publication is disabled: ABI1 baseline identity is invalid"
        )
    if baseline.get("state") != "active":
        raise SystemExit(
            "release publication is disabled: ABI1 baseline is not active "
            f"(state={baseline.get('state')!r})"
        )


def repository_license(repo: Path) -> bytes:
    matches = [
        repo / name
        for name in ("LICENSE", "LICENSE.txt", "LICENSE.md")
        if (repo / name).is_file()
    ]
    if len(matches) != 1:
        raise SystemExit(
            "registry publication requires exactly one canonical root license"
        )
    return matches[0].read_bytes()


def validate_release_inputs(
    root: Path,
    tag: str,
    npm_version: str,
    python_version: str,
    commit: str | None = None,
) -> set[str]:
    provenance_files = sorted(root.rglob("*.provenance.json"))
    sbom_files = sorted(root.rglob("*.spdx.json"))
    targets: set[str] = set()
    provenance_by_target: dict[str, dict[str, Any]] = {}
    build_tools: set[tuple[str, str]] = set()
    for path in provenance_files:
        value = json.loads(path.read_text(encoding="utf-8"))
        target = value.get("target", {})
        package_name = target.get("packageName")
        if (
            value.get("schemaVersion") != 2
            or value.get("abiMajor") != 1
            or value.get("releaseTag") != tag
            or value.get("npmVersion") != npm_version
            or value.get("pythonVersion") != python_version
            or package_name not in NATIVE_PACKAGES
        ):
            raise SystemExit(f"{path}: provenance identity does not match the release")
        if not isinstance(value.get("sourceSha256"), str) or not re.fullmatch(
            r"[0-9a-f]{64}", value["sourceSha256"]
        ):
            raise SystemExit(f"{path}: provenance source SHA is missing or invalid")
        expected_dynamic_identity = (
            {"kind": "install_name", "value": "@rpath/libtny.1.dylib"}
            if package_name == "@thehumanworks/tny-darwin-arm64"
            else {"kind": "soname", "value": "libtny.so.1"}
        )
        if value.get("dynamicIdentity") != expected_dynamic_identity:
            raise SystemExit(f"{path}: provenance dynamic identity is invalid")
        observed_commit = value.get("commitSha")
        if not isinstance(observed_commit, str) or not re.fullmatch(
            r"[0-9a-f]{40}", observed_commit
        ):
            raise SystemExit(f"{path}: provenance commit SHA is missing or invalid")
        if commit is not None and observed_commit != commit:
            raise SystemExit(f"{path}: provenance commit SHA does not match GITHUB_SHA")
        tools = value.get("buildTools")
        if (
            not isinstance(tools, dict)
            or not isinstance(tools.get("node"), str)
            or not isinstance(tools.get("npm"), str)
        ):
            raise SystemExit(f"{path}: provenance build-tool identity is missing")
        build_tools.add((tools["node"], tools["npm"]))
        if not value.get("contents", {}).get("meta") or not value.get(
            "contents", {}
        ).get("native"):
            raise SystemExit(f"{path}: npm dry-run content report is empty")
        packages = value.get("packages")
        if not isinstance(packages, list) or len(packages) != 2:
            raise SystemExit(f"{path}: provenance package set is incomplete")
        for package in packages:
            artifact = path.parent / str(package.get("name"))
            if not artifact.is_file() or digest(artifact.read_bytes()) != package.get(
                "sha256"
            ):
                raise SystemExit(f"{path}: provenance package hash is invalid")
            with tarfile.open(artifact, "r:gz") as archive:
                actual_contents = sorted(
                    member.name.removeprefix("package/")
                    for member in archive.getmembers()
                    if member.isfile()
                )
            content_key = (
                "meta" if package.get("role") == "npm-meta-package" else "native"
            )
            if actual_contents != value.get("contents", {}).get(content_key):
                raise SystemExit(
                    f"{path}: provenance {content_key} content report is invalid"
                )
        native_package = next(
            (item for item in packages if item.get("role") == "npm-native-package"),
            None,
        )
        native_package_path = path.parent / str(
            native_package.get("name") if isinstance(native_package, dict) else ""
        )
        if not native_package_path.is_file():
            raise SystemExit(f"{path}: provenance native package is missing")
        with tarfile.open(native_package_path, "r:gz") as archive:
            for artifact in value.get("nativeArtifacts", []):
                try:
                    artifact_data = tar_member(
                        archive, str(artifact.get("packagePath"))
                    )
                except (KeyError, SystemExit):
                    raise SystemExit(
                        f"{path}: provenance native artifact is missing"
                    ) from None
                if digest(artifact_data) != artifact.get("sha256"):
                    raise SystemExit(
                        f"{path}: provenance native artifact hash is invalid"
                    )
        wheel = value.get("pythonWheel")
        wheel_path = path.parent / str(
            wheel.get("name") if isinstance(wheel, dict) else ""
        )
        if (
            not isinstance(wheel, dict)
            or not wheel_path.is_file()
            or digest(wheel_path.read_bytes()) != wheel.get("sha256")
            or not value.get("contents", {}).get("wheel")
        ):
            raise SystemExit(f"{path}: provenance wheel hash/content report is invalid")
        expected_wheel_markers = {
            "@thehumanworks/tny-darwin-arm64": ("macosx_13_0", "arm64.whl"),
            "@thehumanworks/tny-linux-arm64": ("manylinux", "aarch64.whl"),
            "@thehumanworks/tny-linux-x64": ("manylinux", "x86_64.whl"),
        }[package_name]
        if not all(marker in wheel_path.name for marker in expected_wheel_markers):
            raise SystemExit(
                f"{path}: provenance wheel target does not match native package"
            )
        with zipfile.ZipFile(wheel_path) as archive:
            if sorted(archive.namelist()) != value["contents"]["wheel"]:
                raise SystemExit(f"{path}: provenance wheel content report is invalid")
        targets.add(package_name)
        provenance_by_target[package_name] = value
    observed_sboms: set[str] = set()
    for path in sbom_files:
        value = json.loads(path.read_text(encoding="utf-8"))
        packages = value.get("packages")
        if (
            value.get("spdxVersion") != "SPDX-2.3"
            or not isinstance(packages, list)
            or len(packages) != 1
        ):
            raise SystemExit(f"{path}: SPDX document structure is invalid")
        package = packages[0]
        name = package.get("name")
        if name not in NATIVE_PACKAGES or package.get("versionInfo") != npm_version:
            raise SystemExit(
                f"{path}: SPDX package identity does not match the release"
            )
        provenance = provenance_by_target.get(name)
        if provenance is None:
            raise SystemExit(f"{path}: SPDX target has no matching provenance")
        if value.get("comment") != f"source commit {provenance['commitSha']}":
            raise SystemExit(f"{path}: SPDX document is not bound to GITHUB_SHA")
        native_package = next(
            item
            for item in provenance["packages"]
            if item["role"] == "npm-native-package"
        )
        package_checksums = {
            item.get("algorithm"): item.get("checksumValue")
            for item in package.get("checksums", [])
        }
        if package_checksums.get("SHA256") != native_package["sha256"]:
            raise SystemExit(f"{path}: SPDX native package checksum is invalid")
        expected_files = {
            item["name"]: item["sha256"]
            for item in [*provenance["nativeArtifacts"], provenance["pythonWheel"]]
        }
        actual_files = {}
        for item in value.get("files", []):
            checksums = {
                checksum.get("algorithm"): checksum.get("checksumValue")
                for checksum in item.get("checksums", [])
            }
            actual_files[item.get("fileName")] = checksums.get("SHA256")
        if actual_files != expected_files:
            raise SystemExit(f"{path}: SPDX file checksums are invalid")
        observed_sboms.add(name)
    if targets != observed_sboms:
        raise SystemExit("provenance and SPDX target sets differ")
    if len(build_tools) > 1:
        raise SystemExit(
            f"native packages used different Node/npm versions: {sorted(build_tools)}"
        )
    return targets


def validate_artifact_cardinality(
    npm_counts: dict[str, int], wheel_targets: list[str], allow_partial: bool
) -> None:
    if allow_partial:
        natives = [name for name in NATIVE_PACKAGES if npm_counts.get(name)]
        if npm_counts and (
            npm_counts.get("@thehumanworks/tny") != 1
            or len(natives) != 1
            or npm_counts[natives[0]] != 1
        ):
            raise SystemExit(
                "partial npm artifact set contains duplicate or missing packages"
            )
        if wheel_targets and len(wheel_targets) != 1:
            raise SystemExit("partial Python artifact set contains duplicate wheels")
        return
    if npm_counts.get("@thehumanworks/tny") != 3:
        raise SystemExit(
            f"expected three identical meta packages, got {npm_counts.get('@thehumanworks/tny', 0)}"
        )
    invalid_natives = sorted(
        name for name in NATIVE_PACKAGES if npm_counts.get(name) != 1
    )
    if invalid_natives:
        raise SystemExit(
            f"native npm package cardinality is invalid: {invalid_natives}"
        )
    expected_wheels = {"darwin-arm64", "linux-x86_64", "linux-aarch64"}
    invalid_wheels = sorted(
        target for target in expected_wheels if wheel_targets.count(target) != 1
    )
    if set(wheel_targets) != expected_wheels or invalid_wheels:
        raise SystemExit(
            f"Python wheel target cardinality is invalid: {invalid_wheels or sorted(set(wheel_targets))}"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--allow-partial", action="store_true")
    parser.add_argument("--require-active", action="store_true")
    parser.add_argument("--require-publishable-license", action="store_true")
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--commit")
    args = parser.parse_args()
    npm_version, python_version = versions(args.tag)
    expected_license = None
    if args.require_active:
        require_active_abi_baseline(args.repo)
    if args.require_publishable_license:
        require_publishable_license(args.repo)
        expected_license = repository_license(args.repo)
    npm_artifacts = sorted(args.root.rglob("*.tgz"))
    wheel_artifacts = sorted(args.root.rglob("*.whl"))
    observed: dict[str, set[str]] = {}
    npm_counts: dict[str, int] = {}
    for artifact in npm_artifacts:
        name, value = validate_npm(artifact, args.tag, npm_version, expected_license)
        observed.setdefault(name, set()).add(value)
        npm_counts[name] = npm_counts.get(name, 0) + 1
    wheel_target_list = [
        validate_wheel(artifact, python_version, expected_license)
        for artifact in wheel_artifacts
    ]
    wheel_targets = set(wheel_target_list)
    input_targets = validate_release_inputs(
        args.root, args.tag, npm_version, python_version, args.commit
    )
    validate_artifact_cardinality(npm_counts, wheel_target_list, args.allow_partial)
    if args.allow_partial and not npm_artifacts and not wheel_artifacts:
        raise SystemExit("SDK release artifact set is empty")
    if args.allow_partial and npm_artifacts:
        if "@thehumanworks/tny" not in observed or not any(
            name in NATIVE_PACKAGES for name in observed
        ):
            raise SystemExit(
                "partial npm artifact set must contain the meta and one native package"
            )
        native_observed = {name for name in observed if name in NATIVE_PACKAGES}
        if input_targets != native_observed:
            raise SystemExit("partial npm package and release-input target sets differ")
    if not args.allow_partial:
        expected = {"@thehumanworks/tny", *NATIVE_PACKAGES}
        if set(observed) != expected:
            raise SystemExit(f"npm package set is incomplete: got {sorted(observed)}")
        if len(observed["@thehumanworks/tny"]) != 1:
            raise SystemExit(
                "npm meta packages built on different runners are not byte-identical"
            )
        if input_targets != set(NATIVE_PACKAGES):
            raise SystemExit(
                f"SDK release input set is incomplete: got {sorted(input_targets)}"
            )
        if not wheel_artifacts:
            raise SystemExit("Python release wheel set is empty")
        expected_wheels = {"darwin-arm64", "linux-x86_64", "linux-aarch64"}
        if wheel_targets != expected_wheels:
            raise SystemExit(
                f"Python wheel platform set is incomplete: got {sorted(wheel_targets)}"
            )
    print(
        json.dumps(
            {
                "tag": args.tag,
                "npm_version": npm_version,
                "python_version": python_version,
                "npm_packages": sorted(observed),
                "wheel_targets": sorted(wheel_targets),
            },
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
