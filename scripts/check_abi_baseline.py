#!/usr/bin/env python3
"""Validate and compare deterministic libtny ABI baseline descriptions."""
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path
from typing import Any


class BaselineError(ValueError):
    pass


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
    if set(value) != {"fixture_base", "changes"} or not isinstance(value.get("changes"), dict):
        raise BaselineError(f"{path}: fixture overlay must contain only fixture_base and changes")
    base_path = (path.parent / fixture_base).resolve()
    return _merge(load(base_path), value["changes"])


def validate(value: dict[str, Any], label: str) -> list[str]:
    errors: list[str] = []
    required = {
        "schema_version", "state", "abi", "data_model", "calling_convention",
        "constants", "structs", "exports", "artifacts", "compatibility",
    }
    missing = sorted(required - value.keys())
    if missing:
        errors.append(f"{label}: missing keys: {', '.join(missing)}")
        return errors
    if value["schema_version"] != 1:
        errors.append(f"{label}: schema_version must be 1")
    abi = value.get("abi")
    if (not isinstance(abi, dict) or abi.get("major") != 1 or
            not isinstance(abi.get("minor"), int) or abi["minor"] < 0 or
            abi.get("encoded") != (1 << 16) | abi["minor"] or
            abi.get("elf_version_node") != f"LIBTNY_1.{abi['minor']}"):
        errors.append(f"{label}: ABI identity/version node is inconsistent")
    constants = value.get("constants")
    if not isinstance(constants, dict) or not constants:
        errors.append(f"{label}: constants must be a non-empty object")
    elif any(not isinstance(name, str) or not isinstance(number, int)
             for name, number in constants.items()):
        errors.append(f"{label}: every constant must map a name to an integer")
    exports = value.get("exports")
    if (not isinstance(exports, dict) or not exports or
            any(not isinstance(name, str) or not isinstance(node, str)
                for name, node in exports.items())):
        errors.append(f"{label}: exports must map symbol names to ELF version nodes")
    elif list(exports) != sorted(exports):
        errors.append(f"{label}: export names must be sorted")
    elif any(not node.startswith("LIBTNY_1.") for node in exports.values()):
        errors.append(f"{label}: every export must use a LIBTNY_1.N node")
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
        if not isinstance(size, int) or size <= 0 or not isinstance(alignment, int) or alignment <= 0:
            errors.append(f"{label}: struct {name} has invalid size/alignment")
            continue
        if size % alignment:
            errors.append(f"{label}: struct {name} size is not alignment-multiple")
        if mode not in {"fixed", "sized_tail"}:
            errors.append(f"{label}: struct {name} has invalid extensibility")
        if not isinstance(minimum_size, int) or minimum_size <= 0 or minimum_size > size:
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
            if (not isinstance(field_name, str) or field_name in names or
                    not isinstance(offset, int) or not isinstance(field_size, int) or
                    offset < prior_end or field_size <= 0 or offset + field_size > size):
                errors.append(f"{label}: struct {name} has invalid field {field!r}")
                continue
            names.add(field_name)
            prior_end = offset + field_size
    return errors


def compare(baseline: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    errors = validate(candidate, "candidate")
    if errors:
        return errors
    for key in ("data_model", "calling_convention", "compatibility"):
        if candidate[key] != baseline[key]:
            errors.append(f"incompatible {key}: expected {baseline[key]!r}, got {candidate[key]!r}")
    if candidate["abi"]["major"] != baseline["abi"]["major"] or candidate["abi"]["minor"] < baseline["abi"]["minor"]:
        errors.append(f"ABI version regressed or changed major: {candidate['abi']!r}")
    expected_linux = baseline["artifacts"]["linux-glibc"]
    actual_linux = candidate["artifacts"].get("linux-glibc", {})
    for key in ("architectures", "soname", "linker_name"):
        if actual_linux.get(key) != expected_linux.get(key):
            errors.append(f"incompatible linux artifact {key}: expected {expected_linux.get(key)!r}, got {actual_linux.get(key)!r}")
    expected_nodes = expected_linux.get("elf_version_nodes", [])
    actual_nodes = actual_linux.get("elf_version_nodes", [])
    if actual_nodes[:len(expected_nodes)] != expected_nodes:
        errors.append(f"ELF version-node ancestry changed: expected prefix {expected_nodes!r}, got {actual_nodes!r}")
    required_node = f"LIBTNY_1.{candidate['abi']['minor']}"
    if not actual_nodes or actual_nodes[-1] != required_node:
        errors.append(f"ELF version-node list must end at {required_node}, got {actual_nodes!r}")
    expected_artifacts = {key: value for key, value in baseline["artifacts"].items() if key != "linux-glibc"}
    actual_artifacts = {key: value for key, value in candidate["artifacts"].items() if key != "linux-glibc"}
    if actual_artifacts != expected_artifacts:
        errors.append(f"incompatible non-Linux artifacts: expected {expected_artifacts!r}, got {actual_artifacts!r}")
    for name, number in sorted(baseline["constants"].items()):
        actual = candidate["constants"].get(name)
        if actual != number:
            errors.append(f"constant {name}: expected {number}, got {actual!r}")
    missing_exports = sorted(set(baseline["exports"]) - set(candidate["exports"]))
    for name in missing_exports:
        errors.append(f"missing export: {name}")
    for name, node in sorted(baseline["exports"].items()):
        if candidate["exports"].get(name) != node:
            errors.append(f"export {name} moved from {node} to {candidate['exports'].get(name)!r}")
    added_exports = sorted(set(candidate["exports"]) - set(baseline["exports"]))
    for name in added_exports:
        if (candidate["abi"]["minor"] <= baseline["abi"]["minor"] or
                candidate["exports"][name] != required_node):
            errors.append(f"new export {name} must enter a later minor node, got {candidate['exports'][name]!r}")
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
        if expected["extensibility"] == "fixed" and actual.get("size") != expected["size"]:
            errors.append(f"struct {name} size: expected exactly {expected['size']}, got {actual.get('size')!r}")
        if expected["extensibility"] == "sized_tail" and actual.get("size", 0) < expected["size"]:
            errors.append(f"struct {name} size shrank below {expected['size']}: {actual.get('size')!r}")
        actual_fields = {field["name"]: field for field in actual.get("fields", [])}
        for field in expected["fields"]:
            got = actual_fields.get(field["name"])
            if got != field:
                errors.append(f"struct {name}.{field['name']}: expected {field!r}, got {got!r}")
        expected_names = {field["name"] for field in expected["fields"]}
        for field in actual.get("fields", []):
            if field["name"] not in expected_names and field["offset"] < expected["append_from"]:
                errors.append(
                    f"struct {name}.{field['name']}: new field offset {field['offset']} "
                    f"precedes append boundary {expected['append_from']}"
                )
    return sorted(set(errors))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, default=Path("abi/baseline-v1.json"))
    parser.add_argument("--candidate", type=Path)
    args = parser.parse_args()
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
