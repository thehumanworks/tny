#!/usr/bin/env python3
"""Validate and compare deterministic libtny ABI baseline descriptions."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


class BaselineError(ValueError):
    pass


def _git_blob(commit: str, path: str) -> bytes:
    completed = subprocess.run(
        ["git", "show", f"{commit}:{path}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise BaselineError(
            f"frozen ABI0 source {commit}:{path} is unavailable; "
            "full git checkouts must retain commit 510a95c and source "
            "tarball builds must set ABI0_COMPAT_ARCHIVE to a separately "
            "verified compatibility source archive"
        )
    return completed.stdout


def validate_compat0(path: Path, source_root: Path | None = None) -> list[str]:
    value = _read(path)
    errors: list[str] = []
    commit = value.get("source_commit")
    initial_commit = "510a95c2ef89aa9ec02a66d8b0a5cadd953025a8"
    initial_archive = "8718336dbde47f3f8427bf6b3a724127e3ed24b61eaedb6f315523ec2a00c2f6"
    lineage = value.get("maintenance_lineage")
    if (
        not isinstance(lineage, list)
        or not lineage
        or lineage[0].get("commit") != initial_commit
        or lineage[0].get("git_archive_sha256") != initial_archive
        or lineage[0].get("review_ref") != "issue-45-initial-ga"
    ):
        errors.append(
            "compat0 maintenance lineage must preserve the 510a95c initial GA identity"
        )
        return errors
    active = [
        entry
        for entry in lineage
        if isinstance(entry, dict) and entry.get("commit") == commit
    ]
    if (
        len(active) != 1
        or not active[0].get("review_ref")
        or value.get("git_archive_sha256") != active[0].get("git_archive_sha256")
    ):
        errors.append(
            "compat0 active source is not an approved hash-pinned maintenance commit"
        )
        return errors
    header_spec = value.get("header", {})
    export_spec = value.get("exports", {})
    blobs: dict[str, bytes] = {}
    for label, blob_path, expected in (
        ("header", header_spec.get("path"), header_spec.get("sha256")),
        ("ELF exports", export_spec.get("elf_path"), export_spec.get("elf_sha256")),
        (
            "macOS exports",
            export_spec.get("macos_path"),
            export_spec.get("macos_sha256"),
        ),
    ):
        if not isinstance(blob_path, str) or not isinstance(expected, str):
            errors.append(f"compat0 {label} path/hash is missing")
            continue
        if source_root is None:
            blob = _git_blob(commit, blob_path)
        else:
            try:
                blob = (source_root / blob_path).read_bytes()
            except OSError as error:
                errors.append(f"compat0 {label} is unavailable: {error}")
                continue
        blobs[label] = blob
        actual = hashlib.sha256(blob).hexdigest()
        if actual != expected:
            errors.append(f"compat0 {label} hash: expected {expected}, got {actual}")
    if errors or "header" not in blobs:
        return errors
    header = blobs["header"].decode("utf-8")
    for define, number in (("TNY_ABI_MAJOR", 0), ("TNY_ABI_MINOR", 8)):
        if not re.search(rf"#define\s+{define}\s+{number}u\b", header):
            errors.append(f"compat0 header does not define {define}={number}")
    header_exports = set(
        re.findall(r"TNY_API\s+[^;]*?\b(tny_[a-z0-9_]+)\s*\(", header, re.DOTALL)
    )
    elf_exports = set(
        re.findall(
            r"^\s+(tny_[a-z0-9_]+);", blobs.get("ELF exports", b"").decode(), re.M
        )
    )
    mac_exports = set(
        re.findall(
            r"^_(tny_[a-z0-9_]+)$", blobs.get("macOS exports", b"").decode(), re.M
        )
    )
    if header_exports != elf_exports or header_exports != mac_exports:
        errors.append("compat0 frozen header and platform export allowlists differ")
    if export_spec.get("elf_version_node") != "LIBTNY_0":
        errors.append("compat0 ELF version node must remain LIBTNY_0")
    artifacts = value.get("artifacts", {})
    expected_artifacts = {
        "linux_soname": "libtny.so.0",
        "macos_install_name": "@rpath/libtny.0.dylib",
        "pkg_config": "libtny-0",
    }
    if artifacts != expected_artifacts:
        errors.append(f"compat0 artifact identity drift: {artifacts!r}")

    compiler = shutil.which("cc")
    structs = value.get("structs", {})
    if compiler is None:
        errors.append("C compiler is required to verify compat0 layouts/signatures")
        return errors
    assertions = ["#include <stddef.h>", '#include "tny/tny.h"']
    for name, dimensions in sorted(structs.items()):
        if (
            not isinstance(dimensions, list)
            or len(dimensions) != 2
            or not all(isinstance(item, int) for item in dimensions)
        ):
            errors.append(f"compat0 struct manifest is invalid for {name}")
            continue
        assertions.append(
            f'_Static_assert(sizeof({name}) == {dimensions[0]}, "{name} size");'
        )
        assertions.append(
            f'_Static_assert(_Alignof({name}) == {dimensions[1]}, "{name} align");'
        )
    declarations = {
        "tny_capabilities_init": "void (TNY_CALL *sig_cap_init)(tny_capabilities_v0 *) = tny_capabilities_init;",
        "tny_capabilities_v1_init": "void (TNY_CALL *sig_cap1_init)(tny_capabilities_v1 *) = tny_capabilities_v1_init;",
        "tny_event_read": "int32_t (TNY_CALL *sig_event_read)(const tny_event *, tny_event_view_v0 *) = tny_event_read;",
        "tny_event_view_init": "void (TNY_CALL *sig_event_init)(tny_event_view_v0 *) = tny_event_view_init;",
        "tny_host_services_v1_init": "void (TNY_CALL *sig_host_init)(tny_host_services_v1 *) = tny_host_services_v1_init;",
        "tny_runtime_create": "int32_t (TNY_CALL *sig_create)(const tny_runtime_options_v0 *, tny_runtime **, tny_error **) = tny_runtime_create;",
        "tny_runtime_create_v1": "int32_t (TNY_CALL *sig_create1)(const tny_runtime_options_v1 *, tny_runtime **, tny_error **) = tny_runtime_create_v1;",
        "tny_runtime_get_capabilities": "int32_t (TNY_CALL *sig_caps)(const tny_runtime *, tny_capabilities_v0 *) = tny_runtime_get_capabilities;",
        "tny_runtime_get_capabilities_v1": "int32_t (TNY_CALL *sig_caps1)(const tny_runtime *, tny_capabilities_v1 *) = tny_runtime_get_capabilities_v1;",
        "tny_runtime_options_init": "void (TNY_CALL *sig_opts)(tny_runtime_options_v0 *) = tny_runtime_options_init;",
        "tny_runtime_options_v1_init": "void (TNY_CALL *sig_opts1)(tny_runtime_options_v1 *) = tny_runtime_options_v1_init;",
        "tny_tool_result_v1_init": "void (TNY_CALL *sig_result)(tny_tool_result_v1 *) = tny_tool_result_v1_init;",
        "tny_tool_spec_v1_init": "void (TNY_CALL *sig_spec)(tny_tool_spec_v1 *) = tny_tool_spec_v1_init;",
    }
    if set(value.get("legacy_signatures", {})) != set(declarations):
        errors.append("compat0 legacy signature manifest names drifted")
    assertions.extend(declarations.values())
    assertions.append("int main(void) { return 0; }")
    with tempfile.TemporaryDirectory() as directory:
        include = Path(directory) / "tny"
        include.mkdir()
        (include / "tny.h").write_bytes(blobs["header"])
        completed = subprocess.run(
            [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                directory,
                "-x",
                "c",
                "-",
                "-fsyntax-only",
            ],
            input="\n".join(assertions),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    if completed.returncode != 0:
        errors.append(
            f"compat0 frozen layout/signature probe failed: {completed.stderr.strip()}"
        )
    return errors


def _numeric_version(value: str, label: str) -> tuple[int, int, int]:
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", value)
    if not match:
        raise BaselineError(f"invalid {label}: {value!r}")
    return tuple(int(item) for item in match.groups())


def macho_version(
    product_version: str,
    previous_version: str = "1.0.0",
    development_fallback: bool = False,
) -> str:
    match = re.fullmatch(
        r"v?(\d+)\.(\d+)\.(\d+)(?:[-+][0-9A-Za-z.-]+)?", product_version
    )
    if not match:
        raise BaselineError(
            f"unrepresentable product SemVer for Mach-O: {product_version!r}"
        )
    parts = tuple(int(value) for value in match.groups())
    if parts[0] > 65535 or parts[1] > 255 or parts[2] > 255:
        raise BaselineError(
            f"Mach-O version component out of range: {product_version!r}"
        )
    previous = _numeric_version(previous_version, "previous Mach-O version")
    if parts < (1, 0, 0):
        if development_fallback and re.fullmatch(
            r"v?\d+\.\d+\.\d+-\d+-g[0-9a-f]+(?:-dirty)?", product_version
        ):
            return "1.0.0"
        raise BaselineError(
            f"product SemVer precedes ABI1 compatibility version: {product_version!r}"
        )
    if parts < previous:
        raise BaselineError(
            f"Mach-O current version regressed from {previous_version} "
            f"to {product_version!r}"
        )
    return ".".join(str(value) for value in parts)


def _read(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BaselineError(f"{path}: invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise BaselineError(f"{path}: root must be an object")
    return value


def _merge(base: Any, changes: Any) -> Any:
    if isinstance(base, dict) and isinstance(changes, dict):
        result = copy.deepcopy(base)
        for key in sorted(changes):
            result[key] = _merge(result.get(key), changes[key])
        return result
    return copy.deepcopy(changes)


def load(path: Path) -> dict[str, Any]:
    value = _read(path)
    fixture_base = value.get("fixture_base")
    if fixture_base is None:
        return value
    if set(value) != {"fixture_base", "changes"} or not isinstance(
        value.get("changes"), dict
    ):
        raise BaselineError(
            f"{path}: fixture overlay must contain only fixture_base and changes"
        )
    base_path = (path.parent / fixture_base).resolve()
    return _merge(load(base_path), value["changes"])


def validate(value: dict[str, Any], label: str) -> list[str]:
    errors: list[str] = []
    required = {
        "schema_version",
        "state",
        "abi",
        "data_model",
        "calling_convention",
        "constants",
        "structs",
        "exports",
        "capacity_signatures",
        "signature_manifest",
        "artifacts",
        "compatibility",
    }
    missing = sorted(required - value.keys())
    if missing:
        errors.append(f"{label}: missing keys: {', '.join(missing)}")
        return errors
    if value["schema_version"] != 1:
        errors.append(f"{label}: schema_version must be 1")
    if value["state"] not in {"candidate-pending-platform-fixtures", "active"}:
        errors.append(f"{label}: invalid ABI 1 baseline state")
    abi = value.get("abi")
    if (
        not isinstance(abi, dict)
        or abi.get("major") != 1
        or not isinstance(abi.get("minor"), int)
        or abi["minor"] < 0
        or abi.get("encoded") != (1 << 16) | abi["minor"]
        or abi.get("elf_version_node") != f"LIBTNY_1.{abi['minor']}"
    ):
        errors.append(f"{label}: ABI identity/version node is inconsistent")
    constants = value.get("constants")
    if not isinstance(constants, dict) or not constants:
        errors.append(f"{label}: constants must be a non-empty object")
    elif any(
        not isinstance(name, str) or not isinstance(number, int)
        for name, number in constants.items()
    ):
        errors.append(f"{label}: every constant must map a name to an integer")
    exports = value.get("exports")
    if (
        not isinstance(exports, dict)
        or not exports
        or any(
            not isinstance(name, str) or not isinstance(node, str)
            for name, node in exports.items()
        )
    ):
        errors.append(f"{label}: exports must map symbol names to ELF version nodes")
    elif list(exports) != sorted(exports):
        errors.append(f"{label}: export names must be sorted")
    elif any(not node.startswith("LIBTNY_1.") for node in exports.values()):
        errors.append(f"{label}: every export must use a LIBTNY_1.N node")
    signatures = value.get("capacity_signatures")
    if (
        not isinstance(signatures, dict)
        or not signatures
        or list(signatures) != sorted(signatures)
        or any(
            name not in exports or not isinstance(signature, str)
            for name, signature in signatures.items()
        )
    ):
        errors.append(f"{label}: capacity_signatures must be sorted exported symbols")
    signature_spec = value.get("signature_manifest", {})
    signature_path = Path.cwd() / str(signature_spec.get("path", ""))
    try:
        signature_data = signature_path.read_bytes()
        signature_value = json.loads(signature_data)
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f"{label}: signature manifest is unreadable: {error}")
        signature_value = {}
        signature_data = b""
    if hashlib.sha256(signature_data).hexdigest() != signature_spec.get("sha256"):
        errors.append(f"{label}: signature manifest hash mismatch")
    if not set(signature_value.get("exports", {})).issubset(set(exports)):
        errors.append(f"{label}: signature manifest contains a non-exported symbol")
    structs = value.get("structs")
    if not isinstance(structs, dict) or not structs:
        errors.append(f"{label}: structs must be a non-empty object")
        return errors
    for name in sorted(structs):
        spec = structs[name]
        if not isinstance(spec, dict):
            errors.append(f"{label}: struct {name} must be an object")
            continue
        size = spec.get("size")
        alignment = spec.get("alignment")
        mode = spec.get("extensibility")
        minimum_size = spec.get("minimum_size")
        fields = spec.get("fields")
        if (
            not isinstance(size, int)
            or size <= 0
            or not isinstance(alignment, int)
            or alignment <= 0
        ):
            errors.append(f"{label}: struct {name} has invalid size/alignment")
            continue
        if size % alignment:
            errors.append(f"{label}: struct {name} size is not alignment-multiple")
        if mode not in {"fixed", "sized_tail"}:
            errors.append(f"{label}: struct {name} has invalid extensibility")
        if (
            not isinstance(minimum_size, int)
            or minimum_size <= 0
            or minimum_size > size
        ):
            errors.append(f"{label}: struct {name} has invalid minimum_size")
        if mode == "sized_tail" and spec.get("append_from") != size:
            errors.append(f"{label}: struct {name} append_from must equal frozen size")
        if not isinstance(fields, list) or not fields:
            errors.append(f"{label}: struct {name} fields must be a non-empty array")
            continue
        prior_end = 0
        names: set[str] = set()
        for field in fields:
            if not isinstance(field, dict):
                errors.append(f"{label}: struct {name} has a non-object field")
                continue
            field_name = field.get("name")
            offset = field.get("offset")
            field_size = field.get("size")
            if (
                not isinstance(field_name, str)
                or field_name in names
                or not isinstance(offset, int)
                or not isinstance(field_size, int)
                or offset < prior_end
                or field_size <= 0
                or offset + field_size > size
            ):
                errors.append(f"{label}: struct {name} has invalid field {field!r}")
                continue
            names.add(field_name)
            prior_end = offset + field_size
    return errors


def compare(baseline: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    errors = validate(candidate, "candidate")
    if errors:
        return errors
    for key in (
        "data_model",
        "calling_convention",
        "compatibility",
        "capacity_signatures",
    ):
        if candidate[key] != baseline[key]:
            errors.append(
                f"incompatible {key}: expected {baseline[key]!r}, got {candidate[key]!r}"
            )
    if (
        candidate["abi"]["major"] != baseline["abi"]["major"]
        or candidate["abi"]["minor"] < baseline["abi"]["minor"]
    ):
        errors.append(f"ABI version regressed or changed major: {candidate['abi']!r}")
    expected_linux = baseline["artifacts"]["linux-glibc"]
    actual_linux = candidate["artifacts"].get("linux-glibc", {})
    for key in ("architectures", "soname", "linker_name"):
        if actual_linux.get(key) != expected_linux.get(key):
            errors.append(
                f"incompatible linux artifact {key}: expected {expected_linux.get(key)!r}, got {actual_linux.get(key)!r}"
            )
    expected_nodes = expected_linux.get("elf_version_nodes", [])
    actual_nodes = actual_linux.get("elf_version_nodes", [])
    if actual_nodes[: len(expected_nodes)] != expected_nodes:
        errors.append(
            f"ELF version-node ancestry changed: expected prefix {expected_nodes!r}, got {actual_nodes!r}"
        )
    required_node = f"LIBTNY_1.{candidate['abi']['minor']}"
    if not actual_nodes or actual_nodes[-1] != required_node:
        errors.append(
            f"ELF version-node list must end at {required_node}, got {actual_nodes!r}"
        )
    expected_macos = baseline["artifacts"].get("macos", {})
    actual_macos = candidate["artifacts"].get("macos", {})
    for key in (
        "architectures",
        "install_name",
        "compatibility_version",
        "current_version_rule",
    ):
        if actual_macos.get(key) != expected_macos.get(key):
            errors.append(
                f"incompatible macOS artifact {key}: expected "
                f"{expected_macos.get(key)!r}, got {actual_macos.get(key)!r}"
            )
    try:
        expected_current = _numeric_version(
            expected_macos["current_version"], "baseline Mach-O version"
        )
        actual_current = _numeric_version(
            actual_macos["current_version"], "candidate Mach-O version"
        )
        if actual_current < expected_current:
            errors.append("candidate Mach-O current version regressed")
    except (BaselineError, KeyError) as error:
        errors.append(str(error))
    if candidate["artifacts"].get("unsupported") != baseline["artifacts"].get(
        "unsupported"
    ):
        errors.append("unsupported artifact policy changed")
    for name, number in sorted(baseline["constants"].items()):
        actual = candidate["constants"].get(name)
        if actual != number:
            errors.append(f"constant {name}: expected {number}, got {actual!r}")
    added_constants = sorted(set(candidate["constants"]) - set(baseline["constants"]))
    if added_constants and candidate["abi"]["minor"] <= baseline["abi"]["minor"]:
        errors.append(
            f"new constants require a later ABI minor: {', '.join(added_constants)}"
        )
    missing_exports = sorted(set(baseline["exports"]) - set(candidate["exports"]))
    for name in missing_exports:
        errors.append(f"missing export: {name}")
    for name, node in sorted(baseline["exports"].items()):
        if candidate["exports"].get(name) != node:
            errors.append(
                f"export {name} moved from {node} to {candidate['exports'].get(name)!r}"
            )
    added_exports = sorted(set(candidate["exports"]) - set(baseline["exports"]))
    for name in added_exports:
        if (
            candidate["abi"]["minor"] <= baseline["abi"]["minor"]
            or candidate["exports"][name] != required_node
        ):
            errors.append(
                f"new export {name} must enter a later minor node, got {candidate['exports'][name]!r}"
            )
    try:
        baseline_signatures = _read(Path.cwd() / baseline["signature_manifest"]["path"])
        candidate_signatures = _read(
            Path.cwd() / candidate["signature_manifest"]["path"]
        )
        for domain in ("exports", "callbacks"):
            for name, signature in baseline_signatures.get(domain, {}).items():
                if candidate_signatures.get(domain, {}).get(name) != signature:
                    errors.append(f"signature changed: {name}")
        added_callbacks = sorted(
            set(candidate_signatures.get("callbacks", {}))
            - set(baseline_signatures.get("callbacks", {}))
        )
        if added_callbacks and candidate["abi"]["minor"] <= baseline["abi"]["minor"]:
            errors.append(
                "new callback typedefs require a later ABI minor: "
                + ", ".join(added_callbacks)
            )
        for name in added_exports:
            if name not in candidate_signatures.get("exports", {}):
                errors.append(f"new export lacks frozen signature: {name}")
    except (BaselineError, KeyError) as error:
        errors.append(str(error))
    for name, expected in sorted(baseline["structs"].items()):
        actual = candidate["structs"].get(name)
        if actual is None:
            errors.append(f"missing struct: {name}")
            continue
        if actual.get("alignment") != expected["alignment"]:
            errors.append(
                f"struct {name} alignment: expected {expected['alignment']}, "
                f"got {actual.get('alignment')!r}"
            )
        if actual.get("minimum_size") != expected["minimum_size"]:
            errors.append(
                f"struct {name} minimum_size: expected {expected['minimum_size']}, "
                f"got {actual.get('minimum_size')!r}"
            )
        if (
            expected["extensibility"] == "fixed"
            and actual.get("size") != expected["size"]
        ):
            errors.append(
                f"struct {name} size: expected exactly {expected['size']}, got {actual.get('size')!r}"
            )
        if (
            expected["extensibility"] == "sized_tail"
            and actual.get("size", 0) < expected["size"]
        ):
            errors.append(
                f"struct {name} size shrank below {expected['size']}: {actual.get('size')!r}"
            )
        actual_fields = {field["name"]: field for field in actual.get("fields", [])}
        for field in expected["fields"]:
            got = actual_fields.get(field["name"])
            if got != field:
                errors.append(
                    f"struct {name}.{field['name']}: expected {field!r}, got {got!r}"
                )
        expected_names = {field["name"] for field in expected["fields"]}
        for field in actual.get("fields", []):
            if (
                field["name"] not in expected_names
                and field["offset"] < expected["append_from"]
            ):
                errors.append(
                    f"struct {name}.{field['name']}: new field offset {field['offset']} "
                    f"precedes append boundary {expected['append_from']}"
                )
    added_structs = sorted(set(candidate["structs"]) - set(baseline["structs"]))
    if added_structs and candidate["abi"]["minor"] <= baseline["abi"]["minor"]:
        errors.append(
            f"new structs require a later ABI minor: {', '.join(added_structs)}"
        )
    return sorted(set(errors))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, default=Path("abi/baseline-v1.json"))
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--compat0", type=Path)
    parser.add_argument("--compat0-source-root", type=Path)
    parser.add_argument("--mach-version")
    parser.add_argument("--development-fallback", action="store_true")
    args = parser.parse_args()
    if args.mach_version is not None:
        try:
            baseline = _read(args.baseline.resolve())
            previous = baseline["artifacts"]["macos"]["current_version"]
            print(macho_version(args.mach_version, previous, args.development_fallback))
            return 0
        except BaselineError as error:
            print(f"ABI baseline error: {error}", file=sys.stderr)
            return 1
    if args.compat0 is not None:
        try:
            errors = validate_compat0(
                args.compat0.resolve(),
                args.compat0_source_root.resolve()
                if args.compat0_source_root
                else None,
            )
        except BaselineError as error:
            errors = [str(error)]
        if errors:
            for error in sorted(set(errors)):
                print(f"ABI baseline error: {error}", file=sys.stderr)
            return 1
        print("libtny ABI 0.8 frozen compatibility manifest: ok")
        return 0
    try:
        baseline = load(args.baseline.resolve())
        errors = validate(baseline, "baseline")
        if not errors and args.candidate:
            errors = compare(baseline, load(args.candidate.resolve()))
    except BaselineError as error:
        errors = [str(error)]
    if errors:
        for error in sorted(set(errors)):
            print(f"ABI baseline error: {error}", file=sys.stderr)
        return 1
    mode = "comparison" if args.candidate else "schema"
    print(f"libtny ABI 1 baseline {mode}: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
