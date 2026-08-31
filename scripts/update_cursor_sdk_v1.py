#!/usr/bin/env python3
"""Vendor and describe the pinned Cursor SDK Bridge sdk.v1 contract.

The default source is GitHub's immutable archive for the release commit.  A
checked-out sdk-bridge tree can be supplied with --source-dir for offline use.
Only Python's standard library is required.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import shutil
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPOSITORY = "https://github.com/cursor/sdk-bridge"
TAG = "v1.0.30"
TAG_OBJECT = "026d21b23641ee488a6650ba850327b8a66ab1cd"
COMMIT = "8157597c625b5f642d3c4a1472d20c9c330a9d18"
VENDOR_REL = Path("third_party/cursor-sdk-bridge") / TAG
INPUT_HASHES = {
    "LICENSE": "d337e05fe93a9bd6c8573b63f8ac14db10e11d897c36df2bfae3a5c5b91bea29",
    "proto/manifest.json": "4ce638a905dc1d2e6067609c89eb259bfb06b14433f5f8bdcb3217d117157293",
    "proto/sdk/v1/sdk_agent_service.proto": "0f4da463f1c95d6627a3cb5131b1926d5abd21d978f560b9c50376f06b460a15",
    "proto/sdk/v1/sdk_bridge_control_service.proto": "a8fbb2303f2286095ec6fb5aa4f179284d43a06d33731ab2b6c3c37eeb53993f",
    "proto/sdk/v1/sdk_cursor_service.proto": "f925cb88412ef0223c9fb968fb86d119c4942c651a50e844f54469031a59beb3",
    "proto/sdk/v1/sdk_custom_tool_callback_service.proto": "29063b363db382827b547b928637ee5c064d5d43ecb6531a7b5aba8086948ecf",
    "proto/sdk/v1/sdk_errors.proto": "6e7165182ec63b563f3bed0bb19634ff073bcf6e562a83f86107e4b8d0ee7ab6",
    "proto/sdk/v1/sdk_messages.proto": "71e571171c3582d24df3169b0c19f194d13d51dbbe8e6508a5a720bc363a4b9d",
    "proto/sdk/v1/sdk_store_callback_service.proto": "a5e6b8727cd4b86dae40f5d6cb0a1b5facf2ca541d45c29e77abb501c43fea13",
}
EXPECTED_COUNTS = {"services": 5, "rpcs": 29, "messages": 114, "fields": 285}
COVERAGE_REL = Path("src/backends/cursor/sdk_v1_coverage.json")

_TOKEN_RE = re.compile(r'"(?:\\.|[^"\\])*"|[A-Za-z_][A-Za-z0-9_]*|-?[0-9]+|[^\s]')


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def _tokens(text: str) -> list[str]:
    return _TOKEN_RE.findall(_strip_comments(text))


def _block_end(tokens: list[str], opening: int) -> int:
    depth = 0
    for i in range(opening, len(tokens)):
        if tokens[i] == "{":
            depth += 1
        elif tokens[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unterminated protobuf declaration")


def _type_name(tokens: list[str]) -> str:
    out = ""
    for token in tokens:
        if token in {"<", ">", "."}:
            out += token
        elif token == ",":
            out += ","
        else:
            out += token
    return out


def _field(statement: list[str], oneof: str | None = None) -> dict[str, object] | None:
    if "=" not in statement:
        return None
    eq = statement.index("=")
    if (
        eq < 2
        or eq + 1 >= len(statement)
        or not re.fullmatch(r"[0-9]+", statement[eq + 1])
    ):
        return None
    prefix = statement[:eq]
    label = "singular"
    if prefix[0] in {"optional", "repeated"}:
        label = prefix.pop(0)
    name = prefix[-1]
    type_tokens = prefix[:-1]
    if not type_tokens or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        return None
    result: dict[str, object] = {
        "name": name,
        "number": int(statement[eq + 1]),
        "type": _type_name(type_tokens),
        "label": label,
    }
    if oneof is not None:
        result["oneof"] = oneof
    return result


def _statements(
    tokens: list[str], start: int, end: int, depth_wanted: int
) -> list[list[str]]:
    result: list[list[str]] = []
    depth = 0
    current: list[str] = []
    for token in tokens[start:end]:
        if token == "{":
            if depth == depth_wanted:
                current = []
            depth += 1
            continue
        elif token == "}":
            depth -= 1
            if depth == depth_wanted:
                current = []
            continue
        elif token == ";" and depth == depth_wanted:
            result.append(current)
            current = []
            continue
        if depth == depth_wanted:
            current.append(token)
    return result


def _message(name: str, file: str, body: list[str]) -> dict[str, object]:
    fields: list[dict[str, object]] = []
    reserved: list[str] = []
    for statement in _statements(body, 0, len(body), 0):
        if statement and statement[0] == "reserved":
            reserved.append(" ".join(statement[1:]))
            continue
        parsed = _field(statement)
        if parsed is not None:
            fields.append(parsed)

    oneofs: list[dict[str, object]] = []
    i = 0
    depth = 0
    while i < len(body):
        if body[i] == "{":
            depth += 1
        elif body[i] == "}":
            depth -= 1
        elif depth == 0 and body[i] == "oneof":
            oneof_name = body[i + 1]
            opening = i + 2
            if body[opening] != "{":
                raise ValueError(f"invalid oneof {oneof_name}")
            closing = _block_end(body, opening)
            oneof_fields = []
            for statement in _statements(body, opening + 1, closing, 0):
                parsed = _field(statement, oneof_name)
                if parsed is not None:
                    fields.append(parsed)
                    oneof_fields.append(parsed["name"])
            oneofs.append({"name": oneof_name, "fields": oneof_fields})
            i = closing
        i += 1
    fields.sort(key=lambda item: int(item["number"]))
    return {
        "name": name,
        "file": file,
        "fields": fields,
        "oneofs": oneofs,
        "reserved": reserved,
    }


def _enum(name: str, file: str, body: list[str]) -> dict[str, object]:
    values = []
    reserved = []
    for statement in _statements(body, 0, len(body), 0):
        if statement and statement[0] == "reserved":
            reserved.append(" ".join(statement[1:]))
        elif "=" in statement:
            eq = statement.index("=")
            if (
                eq == 1
                and eq + 1 < len(statement)
                and re.fullmatch(r"-?[0-9]+", statement[eq + 1])
            ):
                values.append({"name": statement[0], "number": int(statement[eq + 1])})
    return {"name": name, "file": file, "values": values, "reserved": reserved}


def _service(name: str, file: str, body: list[str]) -> dict[str, object]:
    rpcs = []
    for statement in _statements(body, 0, len(body), 0):
        if not statement or statement[0] != "rpc":
            continue
        # rpc Name ([stream] Request) returns ([stream] Response)
        rpc_name = statement[1]
        left = statement.index("(")
        right = statement.index(")", left)
        returns = statement.index("returns", right)
        out_left = statement.index("(", returns)
        out_right = statement.index(")", out_left)
        request_tokens = statement[left + 1 : right]
        response_tokens = statement[out_left + 1 : out_right]
        client_streaming = bool(request_tokens and request_tokens[0] == "stream")
        server_streaming = bool(response_tokens and response_tokens[0] == "stream")
        if client_streaming:
            request_tokens = request_tokens[1:]
        if server_streaming:
            response_tokens = response_tokens[1:]
        rpcs.append(
            {
                "name": rpc_name,
                "request": _type_name(request_tokens),
                "response": _type_name(response_tokens),
                "client_streaming": client_streaming,
                "server_streaming": server_streaming,
            }
        )
    return {"name": name, "file": file, "rpcs": rpcs}


def parse_proto(
    text: str, file: str
) -> tuple[list[dict[str, object]], list[dict[str, object]], list[dict[str, object]]]:
    tokens = _tokens(text)
    services: list[dict[str, object]] = []
    messages: list[dict[str, object]] = []
    enums: list[dict[str, object]] = []
    i = 0
    depth = 0
    while i < len(tokens):
        token = tokens[i]
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
        elif depth == 0 and token in {"service", "message", "enum"}:
            name = tokens[i + 1]
            opening = i + 2
            if tokens[opening] != "{":
                raise ValueError(f"invalid {token} declaration {name} in {file}")
            closing = _block_end(tokens, opening)
            body = tokens[opening + 1 : closing]
            if token == "service":
                services.append(_service(name, file, body))
            elif token == "message":
                messages.append(_message(name, file, body))
            else:
                enums.append(_enum(name, file, body))
            i = closing
        i += 1
    return services, messages, enums


def build_contract(source: Path) -> dict[str, object]:
    manifest = json.loads((source / "proto/manifest.json").read_text(encoding="utf-8"))
    services: list[dict[str, object]] = []
    messages: list[dict[str, object]] = []
    enums: list[dict[str, object]] = []
    files = []
    for relative, expected_hash in sorted(INPUT_HASHES.items()):
        data = (source / relative).read_bytes()
        actual_hash = sha256(data)
        if actual_hash != expected_hash:
            raise ValueError(
                f"{relative}: expected sha256 {expected_hash}, got {actual_hash}"
            )
        files.append({"path": relative, "sha256": actual_hash, "size": len(data)})
        if relative.endswith(".proto"):
            parsed_services, parsed_messages, parsed_enums = parse_proto(
                data.decode("utf-8"), relative
            )
            services.extend(parsed_services)
            messages.extend(parsed_messages)
            enums.extend(parsed_enums)
    services.sort(key=lambda item: (str(item["file"]), str(item["name"])))
    messages.sort(key=lambda item: (str(item["file"]), str(item["name"])))
    enums.sort(key=lambda item: (str(item["file"]), str(item["name"])))
    counts = {
        "services": len(services),
        "rpcs": sum(len(item["rpcs"]) for item in services),
        "messages": len(messages),
        "fields": sum(len(item["fields"]) for item in messages),
        "enums": len(enums),
        "enum_values": sum(len(item["values"]) for item in enums),
        "oneofs": sum(len(item["oneofs"]) for item in messages),
        "reserved_declarations": sum(len(item["reserved"]) for item in messages)
        + sum(len(item["reserved"]) for item in enums),
    }
    for key, expected in EXPECTED_COUNTS.items():
        if counts[key] != expected:
            raise ValueError(
                f"contract count {key}: expected {expected}, got {counts[key]}"
            )
    if manifest != {
        "protocol": "sdk.v1",
        "sdkVersion": "1.0.30",
        "sourceRepo": "anysphere/everysphere",
        "sourceCommit": "a401fe7f346d4d3ba66fd596cc842b0ad5e5259c",
    }:
        raise ValueError(
            "upstream proto/manifest.json identity does not match the pinned release"
        )
    return {
        "schema_version": 1,
        "upstream": {
            "repository": REPOSITORY,
            "tag": TAG,
            "tag_object": TAG_OBJECT,
            "commit": COMMIT,
            "manifest": manifest,
        },
        "counts": counts,
        "files": files,
        "services": services,
        "messages": messages,
        "enums": enums,
    }


def contract_bytes(contract: dict[str, object]) -> bytes:
    return (json.dumps(contract, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def _message_implementation(message: dict[str, object]) -> dict[str, object]:
    """Suggest a starting strategy; a human must review the coverage file."""
    file = str(message["file"])
    name = str(message["name"])
    if file.endswith("sdk_custom_tool_callback_service.proto"):
        return {
            "strategy": "authenticated custom-tool callback JSON dispatch",
            "symbols": ["src/backends/cursor/callbacks.c:tool_request"],
            "tests": [
                "tests/test_cursor_callbacks.c:cursor_callbacks_route_auth_metadata_and_tools"
            ],
        }
    if file.endswith("sdk_store_callback_service.proto"):
        return {
            "strategy": "authenticated durable-store callback JSON dispatch",
            "symbols": ["src/backends/cursor/callbacks.c:store_request"],
            "tests": [
                "tests/test_cursor_callbacks.c:cursor_callback_store_persists_bare_records_blobs_and_events"
            ],
        }
    if file.endswith("sdk_errors.proto"):
        return {
            "strategy": "bounded Connect JSON and protobuf error-detail decoding",
            "symbols": ["src/backends/cursor/sdk_error.c:cursor_sdk_error_parse"],
            "tests": [
                "tests/test_cursor_sdk.c:sdk_v1_structured_error_decodes_all_supported_metadata"
            ],
        }
    if name in {"SendRequest", "ObserveRunRequest", "DownloadArtifactRequest"}:
        return {
            "strategy": "bounded protojson request composition for an outbound sdk.v1 server stream",
            "symbols": ["src/backends/cursor/sdk_client.c:cursor_sdk_invoke_stream"],
            "tests": ["tests/integration/test_cursor_management.py:main"],
        }
    if name == "DownloadArtifactChunk":
        return {
            "strategy": "strict incremental artifact server-stream base64 decoding",
            "symbols": ["src/cli/cmd_cursor.c:cursor_cli_artifact_frame"],
            "tests": [
                "tests/test_cursor_management.c:cursor_artifact_frame_reports_every_boundary_and_failure"
            ],
        }
    if name in {
        "UserMessage",
        "SdkImage",
        "SdkImageUrl",
        "SdkImageData",
        "SdkImageDimension",
    }:
        return {
            "strategy": "bounded Cursor user-message and native image-data composition for the Send server stream",
            "symbols": [
                "src/backends/cursor/cursor.c:cu_send",
                "src/backends/cursor/cursor.c:cu_append_images",
            ],
            "tests": [
                "tests/test_cursor.c:cursor_images_enforce_count_and_encoded_request_limit"
            ],
        }
    if name in {"SendOptions", "LocalSendOptions", "CloudSendOptions"}:
        return {
            "strategy": "validated forward-compatible SendOptions composition for the Send server stream",
            "symbols": ["src/backends/cursor/options.c:cursor_options_send_json"],
            "tests": [
                "tests/test_cursor_options.c:cloud_and_send_options_pass_through"
            ],
        }
    if name in {
        "AgentOptions",
        "LocalAgentOptions",
        "CloudAgentOptions",
        "ModelSelection",
        "ModelParameter",
        "McpServerConfig",
        "AgentDefinition",
        "AgentModeOption",
        "ToolList",
        "SandboxOptions",
        "LocalAgentStoreConfig",
    }:
        return {
            "strategy": "validated forward-compatible options JSON composition",
            "symbols": ["src/backends/cursor/options.c:cursor_options_agent_json"],
            "tests": [
                "tests/test_cursor_options.c:agent_pass_through_preserves_presence_sensitive_tools"
            ],
        }
    if name.startswith("Run") or name in {
        "SdkMessage",
        "InteractionUpdate",
        "ConversationStep",
        "TokenUsage",
        "UsageCost",
        "AgentUsage",
    }:
        return {
            "strategy": "forward-compatible stream envelope and lifecycle event mapping",
            "symbols": ["src/backends/cursor/map.c:cu_on_frame"],
            "tests": [
                "tests/test_cursor.c:interaction_and_step_typed_structs_map_without_inner_type"
            ],
        }
    return {
        "strategy": "bounded sdk.v1 JSON pass-through through the enumerated route client",
        "symbols": ["src/backends/cursor/sdk_client.c:cursor_sdk_invoke_unary"],
        "tests": [
            "tests/test_cursor_sdk.c:sdk_v1_route_table_is_complete_and_lookup_is_stable"
        ],
    }


def _message_wire_roles(contract: dict[str, object]) -> dict[str, list[str]]:
    roles = {str(message["name"]): set() for message in contract["messages"]}
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
            roles[str(rpc["request"])].add(f"{prefix}-request")
            roles[str(rpc["response"])].add(f"{prefix}-response")
    return {name: sorted(value) for name, value in roles.items()}


def build_coverage(contract: dict[str, object]) -> dict[str, object]:
    """Build an intentionally failing review skeleton, never a completion claim."""
    wire_roles = _message_wire_roles(contract)
    rpcs = []
    for service in contract["services"]:
        service_name = str(service["name"])
        callback = service_name in {
            "SdkCustomToolCallbackService",
            "SdkStoreCallbackService",
        }
        for rpc in service["rpcs"]:
            name = str(rpc["name"])
            strategy = (
                "inbound callback"
                if callback
                else (
                    "outbound server stream"
                    if rpc["server_streaming"]
                    else "outbound unary"
                )
            )
            rpcs.append(
                {
                    "service": service_name,
                    "name": name,
                    "request": rpc["request"],
                    "response": rpc["response"],
                    "client_streaming": rpc["client_streaming"],
                    "server_streaming": rpc["server_streaming"],
                    "direction": "inbound" if callback else "outbound",
                    "reviewed": False,
                    "state": "needs-review",
                    "strategy": f"TODO review: {strategy}",
                    "symbols": [],
                    "tests": [],
                    "product_surfaces": [],
                }
            )
    messages = []
    for message in contract["messages"]:
        implementation = _message_implementation(message)
        fields = []
        for field in message["fields"]:
            fields.append(
                {
                    "name": field["name"],
                    "number": field["number"],
                    "reviewed": False,
                    "state": "needs-review",
                    "strategy": f"TODO review: {implementation['strategy']}",
                    "symbols": [],
                    "tests": [],
                    "product_surfaces": [],
                }
            )
        messages.append(
            {
                "file": message["file"],
                "name": message["name"],
                "wire_roles": wire_roles[str(message["name"])],
                "reviewed": False,
                "state": "needs-review",
                "strategy": f"TODO review: {implementation['strategy']}",
                "symbols": [],
                "tests": [],
                "product_surfaces": [],
                "fields": fields,
            }
        )
    return {
        "schema_version": 1,
        "reviewed": False,
        "contract_tag": TAG,
        "contract_commit": COMMIT,
        "rpcs": rpcs,
        "messages": messages,
        "callback_paths": {
            "SdkCustomToolCallbackService.CallCustomTool": "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool",
            "SdkStoreCallbackService.CallStore": "/sdk.v1.SdkStoreCallbackService/CallStore",
        },
    }


def coverage_bytes(coverage: dict[str, object]) -> bytes:
    return (json.dumps(coverage, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def _download_source() -> tuple[tempfile.TemporaryDirectory[str], Path]:
    holder = tempfile.TemporaryDirectory(prefix="cursor-sdk-bridge-")
    url = f"{REPOSITORY}/archive/{COMMIT}.tar.gz"
    with urllib.request.urlopen(url, timeout=60) as response:
        archive = response.read()
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        prefix = f"sdk-bridge-{COMMIT}/"
        wanted = {prefix + relative for relative in INPUT_HASHES}
        members = [
            member
            for member in tar.getmembers()
            if member.name in wanted and member.isfile()
        ]
        if {member.name for member in members} != wanted:
            holder.cleanup()
            raise ValueError("immutable upstream archive is missing pinned inputs")
        tar.extractall(holder.name, members=members, filter="data")
    return holder, Path(holder.name) / f"sdk-bridge-{COMMIT}"


def update(repo_root: Path, source: Path) -> Path:
    contract = build_contract(source)
    destination = repo_root / VENDOR_REL
    destination.mkdir(parents=True, exist_ok=True)
    for relative in INPUT_HASHES:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / relative, target)
    (destination / "contract.json").write_bytes(contract_bytes(contract))
    coverage_path = repo_root / COVERAGE_REL
    coverage_path.parent.mkdir(parents=True, exist_ok=True)
    # Coverage is a reviewed implementation assertion, not generated truth.
    # Seed it only for a new checkout; an SDK update leaves the reviewed file
    # stale so check_cursor_sdk_v1.py forces deliberate reconciliation.
    if not coverage_path.exists():
        coverage_path.write_bytes(coverage_bytes(build_coverage(contract)))
    readme = f"""# Cursor SDK Bridge {TAG}\n\nPinned protocol inputs from [{REPOSITORY}]({REPOSITORY}) at annotated tag\n`{TAG}` (commit `{COMMIT}`, tag object `{TAG_OBJECT}`).\n\n`contract.json` is deterministically generated by `scripts/update_cursor_sdk_v1.py`\nand verified by `scripts/check_cursor_sdk_v1.py`. Do not edit vendored inputs or the\ncontract by hand. The upstream code and schema are licensed under the included\n`LICENSE`.\n"""
    (destination / "README.md").write_text(readme, encoding="utf-8")
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, help="checked-out sdk-bridge root")
    parser.add_argument(
        "--repo-root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    args = parser.parse_args()
    holder = None
    try:
        if args.source_dir is None:
            holder, source = _download_source()
        else:
            source = args.source_dir.resolve()
        destination = update(args.repo_root.resolve(), source)
        print(f"updated {destination} from {TAG} ({COMMIT})")
        return 0
    finally:
        if holder is not None:
            holder.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
