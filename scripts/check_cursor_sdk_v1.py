#!/usr/bin/env python3
"""Verify the vendored Cursor SDK Bridge sdk.v1 inputs and contract."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from update_cursor_sdk_v1 import (
    COVERAGE_REL,
    INPUT_HASHES,
    VENDOR_REL,
    build_contract,
    contract_bytes,
)

_INCOMPLETE = re.compile(r"\b(todo|partial|later|deferred|unimplemented)\b", re.I)
_UNARY = re.compile(r"\bunary\b", re.I)


def _message_wire_roles(
    contract: dict[str, object],
) -> dict[tuple[str, str], list[str]]:
    """Return the direct RPC wire roles for each declared message.

    Nested messages deliberately have an empty list: their implementation
    evidence is reviewed separately, while request/response transport claims
    are mechanically tied to the authoritative service declarations.
    """
    by_name = {
        message["name"]: (message["file"], message["name"])
        for message in contract["messages"]
    }
    roles: dict[tuple[str, str], set[str]] = {key: set() for key in by_name.values()}
    callback_services = {"SdkCustomToolCallbackService", "SdkStoreCallbackService"}
    for service in contract["services"]:
        callback = service["name"] in callback_services
        for rpc in service["rpcs"]:
            if callback:
                prefix = "inbound-callback"
            elif rpc["server_streaming"]:
                prefix = "outbound-server-stream"
            else:
                prefix = "outbound-unary"
            roles[by_name[rpc["request"]]].add(f"{prefix}-request")
            roles[by_name[rpc["response"]]].add(f"{prefix}-response")
    return {key: sorted(value) for key, value in roles.items()}


def _heading_slug(heading: str) -> str:
    heading = heading.strip().lower()
    heading = re.sub(r"[^\w\- ]", "", heading)
    return re.sub(r"[ -]+", "-", heading).strip("-")


def _check_reference(repo_root: Path, reference: str, kind: str) -> str | None:
    separator = "#" if kind == "product" else ":"
    if separator not in reference:
        return f"{kind} reference lacks a concrete marker: {reference}"
    relative, marker = reference.split(separator, 1)
    path = repo_root / relative
    if not relative or not marker or path.resolve().is_relative_to(repo_root) is False:
        return f"invalid {kind} reference: {reference}"
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return f"stale {kind} path: {relative}"
    if kind == "product":
        headings = {
            _heading_slug(match.group(1))
            for match in re.finditer(r"^#{1,6}\s+(.+?)\s*$", text, re.MULTILINE)
        }
        if marker not in headings:
            return f"stale product anchor: {reference}"
    elif re.search(rf"\b{re.escape(marker)}\b", text) is None:
        return f"stale {kind} symbol: {reference}"
    return None


def _validate_coverage(repo_root: Path, contract: dict[str, object]) -> list[str]:
    path = repo_root / COVERAGE_REL
    try:
        raw = path.read_bytes()
        coverage = json.loads(raw)
    except (OSError, ValueError) as error:
        return [f"cannot read implementation coverage: {error}"]
    errors: list[str] = []
    if coverage.get("reviewed") is not True:
        errors.append("implementation coverage has not been explicitly reviewed")
    if (
        coverage.get("contract_tag") != contract["upstream"]["tag"]
        or coverage.get("contract_commit") != contract["upstream"]["commit"]
    ):
        errors.append("implementation coverage is pinned to a different contract")

    expected_rpcs = {
        (
            service["name"],
            rpc["name"],
            rpc["request"],
            rpc["response"],
            rpc["client_streaming"],
            rpc["server_streaming"],
        )
        for service in contract["services"]
        for rpc in service["rpcs"]
    }
    actual_rpcs = {
        (
            rpc.get("service"),
            rpc.get("name"),
            rpc.get("request"),
            rpc.get("response"),
            rpc.get("client_streaming"),
            rpc.get("server_streaming"),
        )
        for rpc in coverage.get("rpcs", [])
        if isinstance(rpc, dict)
    }
    if actual_rpcs != expected_rpcs or len(coverage.get("rpcs", [])) != len(
        expected_rpcs
    ):
        errors.append(
            "implementation coverage RPC set does not exactly match contract.json"
        )

    expected_messages = {
        (message["file"], message["name"]) for message in contract["messages"]
    }
    actual_messages = {
        (message.get("file"), message.get("name"))
        for message in coverage.get("messages", [])
        if isinstance(message, dict)
    }
    if actual_messages != expected_messages or len(coverage.get("messages", [])) != len(
        expected_messages
    ):
        errors.append(
            "implementation coverage message set does not exactly match contract.json"
        )
    contract_messages = {
        (message["file"], message["name"]): message for message in contract["messages"]
    }
    expected_wire_roles = _message_wire_roles(contract)
    for message in coverage.get("messages", []):
        if not isinstance(message, dict):
            continue
        key = (message.get("file"), message.get("name"))
        source = contract_messages.get(key)
        if not source:
            continue
        if message.get("wire_roles") != expected_wire_roles[key]:
            errors.append(f"implementation coverage wire roles do not match {key[1]}")
        expected_fields = {
            (field["name"], field["number"]) for field in source["fields"]
        }
        fields = message.get("fields", [])
        actual_fields = {
            (field.get("name"), field.get("number"))
            for field in fields
            if isinstance(field, dict)
        }
        if actual_fields != expected_fields or len(fields) != len(expected_fields):
            errors.append(
                f"implementation coverage fields do not exactly match {key[1]}"
            )

    entries = list(coverage.get("rpcs", [])) + list(coverage.get("messages", []))
    entries += [
        field
        for message in coverage.get("messages", [])
        if isinstance(message, dict)
        for field in message.get("fields", [])
    ]
    for entry in entries:
        if not isinstance(entry, dict):
            errors.append("implementation coverage contains a non-object entry")
            continue
        label = entry.get("name", "?")
        if entry.get("reviewed") is not True:
            errors.append(f"coverage entry {label} has not been explicitly reviewed")
        if entry.get("state") != "complete":
            errors.append(f"coverage entry {label} is not complete")
        strategy = entry.get("strategy")
        if (
            not isinstance(strategy, str)
            or not strategy.strip()
            or _INCOMPLETE.search(strategy)
        ):
            errors.append(f"coverage entry {label} has an incomplete strategy")
        for key, kind in (
            ("symbols", "symbol"),
            ("tests", "test"),
            ("product_surfaces", "product"),
        ):
            references = entry.get(key)
            if not isinstance(references, list) or not references:
                errors.append(f"coverage entry {label} lacks {key}")
                continue
            for reference in references:
                issue = (
                    _check_reference(repo_root, reference, kind)
                    if isinstance(reference, str)
                    else f"invalid {kind} reference on {label}"
                )
                if issue:
                    errors.append(issue)

    messages_by_name = {
        message.get("name"): message
        for message in coverage.get("messages", [])
        if isinstance(message, dict)
    }
    for name, message in messages_by_name.items():
        roles = message.get("wire_roles", [])
        stream_only = bool(roles) and all(
            role.startswith("outbound-server-stream-") for role in roles
        )
        references = message.get("symbols", [])
        strategy = message.get("strategy", "")
        if stream_only and (
            any("cursor_sdk_invoke_unary" in reference for reference in references)
            or _UNARY.search(strategy)
        ):
            errors.append(
                f"server-stream message {name} claims unary implementation coverage"
            )
        if stream_only:
            for field in message.get("fields", []):
                field_references = field.get("symbols", [])
                field_strategy = field.get("strategy", "")
                if any(
                    "cursor_sdk_invoke_unary" in reference
                    for reference in field_references
                ) or _UNARY.search(field_strategy):
                    errors.append(
                        f"server-stream field {name}.{field.get('name', '?')} "
                        "claims unary implementation coverage"
                    )

    semantic_requirements = {
        "RunStreamMessage": ("src/backends/cursor/map.c:cu_on_frame",),
        "DownloadArtifactChunk": ("src/cli/cmd_cursor.c:cursor_cli_artifact_frame",),
        "UserMessage": (
            "src/backends/cursor/cursor.c:cu_send",
            "src/backends/cursor/cursor.c:cu_append_images",
        ),
        "SendOptions": ("src/backends/cursor/options.c:cursor_options_send_json",),
        "LocalSendOptions": ("src/backends/cursor/options.c:cursor_options_send_json",),
        "CloudSendOptions": ("src/backends/cursor/options.c:cursor_options_send_json",),
    }
    for name, required in semantic_requirements.items():
        symbols = messages_by_name.get(name, {}).get("symbols", [])
        missing = [reference for reference in required if reference not in symbols]
        if missing:
            errors.append(
                f"implementation coverage for {name} lacks semantic evidence: "
                + ", ".join(missing)
            )

    user_message = messages_by_name.get("UserMessage", {})
    user_fields = {
        field.get("name"): field
        for field in user_message.get("fields", [])
        if isinstance(field, dict)
    }
    image_symbols = user_fields.get("images", {}).get("symbols", [])
    if "src/backends/cursor/cursor.c:cu_append_images" not in image_symbols:
        errors.append("UserMessage.images lacks native image composition evidence")

    source = (repo_root / "src/backends/cursor/sdk_client.c").read_text(
        encoding="utf-8"
    )
    route_pattern = re.compile(
        r"\{\s*CURSOR_SDK_RPC_[A-Z0-9_]+\s*,\s*(CONTROL|CURSOR|AGENT)\s*,\s*"
        r'"([A-Za-z0-9]+)"\s*,\s*(CURSOR_SDK_UNARY|CURSOR_SDK_SERVER_STREAM)',
        re.MULTILINE,
    )
    service_paths = {
        "CONTROL": "/sdk.v1.SdkBridgeControlService",
        "CURSOR": "/sdk.v1.SdkCursorService",
        "AGENT": "/sdk.v1.SdkAgentService",
    }
    declared_routes = {
        (service_paths[service], method, kind == "CURSOR_SDK_SERVER_STREAM")
        for service, method, kind in route_pattern.findall(source)
    }
    callback_services = {"SdkCustomToolCallbackService", "SdkStoreCallbackService"}
    expected_routes = {
        (f"/sdk.v1.{service['name']}", rpc["name"], rpc["server_streaming"])
        for service in contract["services"]
        if service["name"] not in callback_services
        for rpc in service["rpcs"]
    }
    if declared_routes != expected_routes or len(declared_routes) != 27:
        errors.append(
            "C route declarations do not exactly match the 27 outbound contract RPCs"
        )

    callback_source = (repo_root / "src/backends/cursor/callbacks.c").read_text(
        encoding="utf-8"
    )
    declared_callback_paths = set(
        re.findall(
            r'^#define\s+(?:TOOL_PATH|STORE_PATH)\s+"([^"\n]+)"',
            callback_source,
            re.MULTILINE,
        )
    )
    expected_callback_paths = {
        f"/sdk.v1.{service['name']}/{rpc['name']}"
        for service in contract["services"]
        if service["name"] in callback_services
        for rpc in service["rpcs"]
    }
    expected_recorded_paths = {
        f"{service['name']}.{rpc['name']}": f"/sdk.v1.{service['name']}/{rpc['name']}"
        for service in contract["services"]
        if service["name"] in callback_services
        for rpc in service["rpcs"]
    }
    recorded_paths = coverage.get("callback_paths", {})
    if (
        declared_callback_paths != expected_callback_paths
        or recorded_paths != expected_recorded_paths
    ):
        errors.append(
            "callback paths do not exactly match the two inbound contract RPCs"
        )
    return list(dict.fromkeys(errors))


def check(repo_root: Path) -> list[str]:
    repo_root = repo_root.resolve()
    vendor = repo_root / VENDOR_REL
    errors: list[str] = []
    expected_files = set(INPUT_HASHES) | {"README.md", "contract.json"}
    if vendor.is_dir():
        actual_files = {
            path.relative_to(vendor).as_posix()
            for path in vendor.rglob("*")
            if path.is_file()
        }
        if actual_files != expected_files:
            missing = sorted(expected_files - actual_files)
            unexpected = sorted(actual_files - expected_files)
            if missing:
                errors.append(f"missing vendored files: {', '.join(missing)}")
            if unexpected:
                errors.append(f"unexpected vendored files: {', '.join(unexpected)}")
    try:
        contract = build_contract(vendor)
        expected = contract_bytes(contract)
    except (OSError, ValueError) as error:
        errors.append(str(error))
        return errors
    try:
        actual = (vendor / "contract.json").read_bytes()
    except OSError as error:
        return [str(error)]
    if actual != expected:
        errors.append(
            f"{vendor / 'contract.json'} is stale or tampered; "
            "run scripts/update_cursor_sdk_v1.py"
        )
    errors.extend(_validate_coverage(repo_root, contract))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    args = parser.parse_args()
    errors = check(args.repo_root.resolve())
    if errors:
        for error in errors:
            print(f"cursor sdk.v1 contract check failed: {error}", file=sys.stderr)
        return 1
    print(
        "cursor sdk.v1 contract check passed (v1.0.30: 5 services, 29 RPCs, 114 messages, 285 fields)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
