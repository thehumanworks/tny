#!/usr/bin/env python3
"""Machine-check the pinned extension parity contract and vocabulary."""

import json
import os
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
TNY = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", ROOT / "build" / "tny"))
MANIFEST = ROOT / "docs" / "features" / "extension-hook-parity.md"
ADR = ROOT / "docs" / "adr" / "0028-extension-parity-contract.md"
SOURCES = ROOT / "docs" / "sources.md"
CAPABILITY_FIXTURE = ROOT / "tests" / "fixtures" / "extension_capabilities.json"

EXPECTED = {
    "Pi": [
        "project_trust", "resources_discover", "session_start",
        "session_info_changed", "session_before_switch", "session_before_fork",
        "session_before_compact", "session_compact", "session_compact_failed",
        "session_shutdown", "session_before_tree", "session_tree", "context",
        "before_provider_request", "before_provider_headers",
        "after_provider_response", "before_agent_start", "agent_start",
        "agent_end", "agent_settled", "turn_start", "turn_end",
        "message_start", "message_update", "message_end",
        "tool_execution_start", "tool_execution_update", "tool_execution_end",
        "model_select", "thinking_level_select", "user_bash", "input",
        "tool_call", "tool_result",
    ],
    "Codex": [
        "PreToolUse", "PermissionRequest", "PostToolUse", "PreCompact",
        "PostCompact", "SessionStart", "SessionEnd", "UserPromptSubmit",
        "SubagentStart", "SubagentStop", "Stop",
    ],
    "Claude": [
        "SessionStart", "Setup", "UserPromptSubmit", "UserPromptExpansion",
        "PreToolUse", "PermissionRequest", "PermissionDenied", "PostToolUse",
        "PostToolUseFailure", "PostToolBatch", "Notification", "MessageDisplay",
        "SubagentStart", "SubagentStop", "TaskCreated", "TaskCompleted", "Stop",
        "StopFailure", "TeammateIdle", "InstructionsLoaded", "ConfigChange",
        "CwdChanged", "DirectoryAdded", "FileChanged", "WorktreeCreate",
        "WorktreeRemove", "PreCompact", "PostCompact", "Elicitation",
        "ElicitationResult", "SessionEnd",
    ],
    "fx": ["PreToolUse", "Stop", "PostTurnEnd", "AttentionRequired"],
}

SECTIONS = {
    "Pi": ("## Pi v0.84.3", "## Codex rust-v0.149.1"),
    "Codex": ("## Codex rust-v0.149.1", "## Claude Code 2.1.245"),
    "Claude": ("## Claude Code 2.1.245", "## fx v0.0.5"),
    "fx": ("## fx v0.0.5", "## Current provider matrices after #55"),
}

CLASSIFICATIONS = {
    "equivalent",
    "equivalent_renamed",
    "observe_only_host_owned",
    "unsupported_safety",
    "operation_absent",
}


def section(text: str, start: str, end: str) -> str:
    assert start in text and end in text
    return text.split(start, 1)[1].split(end, 1)[0]


def table_rows(value: str):
    for line in value.splitlines():
        if not line.startswith("| `"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) == 6:
            yield cells


def main() -> int:
    manifest = MANIFEST.read_text(encoding="utf-8")
    adr = ADR.read_text(encoding="utf-8")
    sources = SOURCES.read_text(encoding="utf-8")
    fixture = json.loads(CAPABILITY_FIXTURE.read_text(encoding="utf-8"))

    for product, hooks in EXPECTED.items():
        body = section(manifest, *SECTIONS[product])
        rows = list(table_rows(body))
        names = [row[0].strip("`") for row in rows]
        assert names == hooks, "%s hook rows differ: %r" % (product, names)
        assert len(names) == len(set(names))
        for row in rows:
            classification = row[2].strip("`")
            assert classification in CLASSIFICATIONS, (product, row)

    pins = (
        "4e58f324fae8ebfa98a3d45181fb248072a2afac",
        "ff29a44391deccde0aba0f8390337d7f3c319ea4",
        "cceab6b3a7a4d899e2a94963852304aaba43d6ac",
        "df7e6245e1992758d4060c97477ceafa27770551",
        "16eda256ca3c94a50744a5fb57d033ec18011f24",
    )
    for pin in pins:
        assert pin in manifest and pin in sources
    assert "dcd461925db2edf69a43c8135db1180d418afd54" not in sources
    assert "rust-v0.148.0" not in sources

    match = re.search(
        r"Every provider matrix contains these independent keys:\n\n```text\n(.*?)\n```",
        adr,
        re.DOTALL,
    )
    assert match
    documented_keys = tuple(line for line in match.group(1).splitlines() if line)
    assert len(documented_keys) == 29
    assert len(documented_keys) == len(set(documented_keys))
    assert tuple(fixture["capabilities"]) == documented_keys

    env = dict(os.environ, TNY_EXTENSIONS="off", TNY_DOCTOR_NO_SPAWN="1")
    completed = subprocess.run(
        [str(TNY), "--provider", "openai", "doctor", "--json"],
        cwd=str(ROOT),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=True,
    )
    capabilities = json.loads(completed.stdout)["extensions"]["capabilities"]
    assert capabilities["schema_version"] == fixture["schema_version"]
    for name, provider in capabilities["providers"].items():
        assert tuple(provider["entries"]) == documented_keys
        expected = fixture["providers"][name]
        assert provider["runtime"] == expected["runtime"]
        assert [entry["state"] for entry in provider["entries"].values()] == expected["states"]
        for key, entry in provider["entries"].items():
            assert entry["state"] in {"supported", "unsupported", "unavailable"}
            if entry["state"] == "supported":
                assert entry["reason"] == "implemented"
            elif entry["state"] == "unavailable":
                assert entry["reason"] == "contracted_not_implemented"
            elif name == "cursor" and key.startswith("extensions.permission."):
                assert entry["reason"] == "protocol_missing"
            else:
                assert entry["reason"] == "provider_owned"

    print("test_extension_contract: all assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
