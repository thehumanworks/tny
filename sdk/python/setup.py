"""Build pure discovery wheels or validated platform wheels with libtny."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import struct
import sys
from pathlib import Path
from typing import Any

from setuptools import Distribution, setup
from setuptools.command.build_py import build_py as _build_py
from setuptools.errors import SetupError
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel

_PACKAGE_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(_PACKAGE_ROOT))
try:
    from release_version import build_version
finally:
    sys.path.pop(0)

SUPPORTED_NAMES = {"libtny.1.dylib", "libtny.so.1"}
MAX_MANIFEST_BYTES = 16 * 1024


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _artifact_target(path: Path) -> tuple[str, str]:
    with path.open("rb") as stream:
        header = stream.read(64)
    if header.startswith(b"\x7fELF"):
        if len(header) < 20 or header[4:6] != b"\x02\x01":
            raise SetupError("bundled libtny artifact has an unsupported format")
        machine = int.from_bytes(header[18:20], "little")
        architecture = {62: "x86_64", 183: "aarch64"}.get(machine)
        if architecture is None or path.name != "libtny.so.1":
            raise SetupError("bundled libtny artifact has an unsupported target")
        return "linux", architecture
    if header.startswith(b"\xcf\xfa\xed\xfe"):
        if len(header) < 8:
            raise SetupError("bundled libtny artifact has an unsupported format")
        cpu_type = struct.unpack_from("<I", header, 4)[0]
        architecture = {0x0100000C: "arm64"}.get(cpu_type)
        if architecture is None or path.name != "libtny.1.dylib":
            raise SetupError("bundled libtny artifact has an unsupported target")
        return "darwin", architecture
    raise SetupError("bundled libtny artifact has an unsupported format")


def _normalized_host() -> tuple[str, str]:
    os_name = platform.system().lower()
    machine = platform.machine().lower()
    architecture = {
        "amd64": "x86_64",
        "x86_64": "x86_64",
        "arm64": "arm64" if os_name == "darwin" else "aarch64",
        "aarch64": "aarch64",
    }.get(machine, machine)
    return os_name, architecture


def _read_manifest() -> dict[str, Any] | None:
    manifest_value = os.environ.get("TNY_BUNDLE_MANIFEST")
    if not manifest_value:
        return None
    try:
        manifest_path = Path(manifest_value).resolve(strict=True)
    except OSError:
        raise SetupError("bundled libtny manifest is invalid") from None
    if manifest_path.stat().st_size > MAX_MANIFEST_BYTES:
        raise SetupError("bundled libtny manifest is invalid")
    try:
        value = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        raise SetupError("bundled libtny manifest is invalid") from None
    if not isinstance(value, dict):
        raise SetupError("bundled libtny manifest is invalid")
    return value


def _validated_bundle() -> Path | None:
    source_value = os.environ.get("TNY_BUNDLE_LIBRARY")
    if not source_value:
        return None
    try:
        source = Path(source_value).resolve(strict=True)
    except OSError:
        raise SetupError("bundled libtny artifact is invalid") from None
    if not source.is_file() or source.name not in SUPPORTED_NAMES:
        raise SetupError("bundled libtny artifact is invalid")
    artifact_os, artifact_arch = _artifact_target(source)
    if (artifact_os, artifact_arch) != _normalized_host():
        raise SetupError("bundled libtny artifact does not match the wheel target")
    digest = _sha256(source)
    expected = os.environ.get("TNY_BUNDLE_SHA256")
    if expected and (len(expected) != 64 or digest != expected.lower()):
        raise SetupError("bundled libtny artifact failed integrity validation")
    manifest = _read_manifest()
    if manifest is not None:
        expected_manifest = {
            "schema_version": 2,
            "filename": source.name,
            "sha256": digest,
            "os": artifact_os,
            "arch": artifact_arch,
            "abi_major": 1,
        }
        if any(manifest.get(key) != value for key, value in expected_manifest.items()):
            raise SetupError("bundled libtny manifest does not match the artifact")
        expected_identity = (
            {"kind": "install_name", "value": "@rpath/libtny.1.dylib"}
            if artifact_os == "darwin"
            else {"kind": "soname", "value": "libtny.so.1"}
        )
        if manifest.get("dynamic_identity") != expected_identity:
            raise SetupError("bundled libtny dynamic identity is invalid")
        if (
            not isinstance(manifest.get("abi_minor"), int)
            or not isinstance(manifest.get("library_version"), str)
            or not manifest["library_version"]
        ):
            raise SetupError("bundled libtny ABI/library version metadata is invalid")
    return source


class TnyDistribution(Distribution):
    def has_ext_modules(self) -> bool:
        return bool(os.environ.get("TNY_BUNDLE_LIBRARY"))


class BuildPy(_build_py):
    def run(self) -> None:
        super().run()
        package_license = Path(self.build_lib, "tny", "LICENSE")
        repository = Path(__file__).resolve().parents[2]
        root_licenses = [
            repository / name
            for name in ("LICENSE", "LICENSE.txt", "LICENSE.md")
            if (repository / name).is_file()
        ]
        if len(root_licenses) > 1:
            raise SetupError("repository has multiple candidate root licenses")
        if root_licenses:
            shutil.copy2(root_licenses[0], package_license)
        elif package_license.exists():
            package_license.unlink()
        target_dir = Path(self.build_lib, "tny", ".libs")
        source = _validated_bundle()
        if target_dir.exists():
            shutil.rmtree(target_dir)
        if source is None:
            return
        target_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target_dir / source.name)


class BdistWheel(_bdist_wheel):
    def finalize_options(self) -> None:
        super().finalize_options()
        self.root_is_pure = not bool(os.environ.get("TNY_BUNDLE_LIBRARY"))

    def get_tag(self) -> tuple[str, str, str]:
        if not os.environ.get("TNY_BUNDLE_LIBRARY"):
            return "py3", "none", "any"
        _python, _abi, platform_tag = super().get_tag()
        return "py3", "none", platform_tag

    def run(self) -> None:
        _validated_bundle()
        super().run()


setup(
    version=build_version(),
    distclass=TnyDistribution,
    cmdclass={"build_py": BuildPy, "bdist_wheel": BdistWheel},
)
