#!/usr/bin/env python3
"""Measure the opt-in request prefix against a local five-request mock turn.

Run: uvx --with tiktoken python tests/bench/measure_prefix.py build/tny
No provider credentials or network inference are used.
"""

import hashlib
import importlib.util
import json
import os
import sys
import tempfile
from pathlib import Path

import tiktoken

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/tny").resolve())
fixture_path = Path(__file__).resolve().parents[1] / "integration/test_prompt_cache.py"
sys.argv = [str(fixture_path), binary]
spec = importlib.util.spec_from_file_location("prefix_mock", fixture_path)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
encoder = tiktoken.get_encoding("o200k_base")
decoder = json.JSONDecoder()


def raw_value(wire, key):
    text = wire.decode()
    marker = json.dumps(key) + ":"
    start = text.index(marker) + len(marker)
    _, end = decoder.raw_decode(text[start:])
    return text[start : start + end]


def measure(enabled):
    case = fixture.CacheTests("test_experimental_prefix_is_stable_for_five_requests")
    case.setUp()
    try:
        case.ws = Path(tempfile.gettempdir()) / "tny-prefix-measure-workspace"
        case.ws.mkdir(parents=True, exist_ok=True)
        case.server.max_tool_steps = 5
        case.env.update(
            OPENAI_BASE_URL=f"http://127.0.0.1:{case.server.server_port}/v1",
            OPENAI_API_KEY="synthetic-prefix-fixture",
        )
        if enabled:
            case.env["TNY_EXP_PREFIX"] = "1"
        case.ask(provider="openai")
        bodies = [body for body, _ in case.server.requests]
        wires = case.server.raw_requests
        assert len(bodies) == len(wires) == 5
        prefixes = []
        for wire in wires:
            setup_raw = ""
            if enabled:
                input_raw = raw_value(wire, "input")
                _, end = decoder.raw_decode(input_raw[1:])
                setup_raw = input_raw[1 : 1 + end]
            prefixes.append(
                raw_value(wire, "tools") + raw_value(wire, "instructions") + setup_raw
            )
        first = bodies[0]
        instructions = first["instructions"]
        tools = raw_value(wires[0], "tools")
        setup = first["input"][0]["content"] if enabled else ""
        section_tokens = {
            "tools": len(encoder.encode(tools)),
            "stable_instructions": len(encoder.encode(instructions)),
            "setup": len(encoder.encode(setup)),
        }
        return {
            "requests": len(bodies),
            "tool_count": len(first["tools"]),
            "tokens": {**section_tokens, "static_total": sum(section_tokens.values())},
            "prefix_byte_identical": all(p == prefixes[0] for p in prefixes),
            "prefix_sha256": hashlib.sha256(prefixes[0].encode()).hexdigest(),
        }
    finally:
        case.doCleanups()


def cross_workspace(enabled):
    case = fixture.CacheTests(
        "test_experimental_first_request_shares_prefix_across_workspaces"
    )
    case.setUp()
    try:
        case.server.max_tool_steps = 1
        if enabled:
            case.env["TNY_EXP_PREFIX"] = "1"
        wires = []
        for suffix in ("a", "b"):
            case.ws = (
                Path(tempfile.gettempdir()) / f"tny-prefix-measure-workspace-{suffix}"
            )
            case.ws.mkdir(parents=True, exist_ok=True)
            case.ask(provider="codex")
            wires.append(case.server.raw_requests[-1])
        shared_bytes = len(os.path.commonprefix(wires))
        shared_tokens = len(
            encoder.encode(wires[0][:shared_bytes].decode(errors="ignore"))
        )
        first_tokens = len(encoder.encode(wires[0].decode()))
        return {
            "provider": "codex local mock",
            "first_request_bytes": len(wires[0]),
            "identical_prefix_bytes": shared_bytes,
            "first_request_tokens_proxy": first_tokens,
            "identical_prefix_tokens_proxy": shared_tokens,
            "expected_first_request_cached_token_fraction_proxy": round(
                shared_tokens / first_tokens, 4
            ),
        }
    finally:
        case.doCleanups()


print(
    json.dumps(
        {
            "flag_off": measure(False),
            "flag_on": measure(True),
            "cross_workspace_flag_off": cross_workspace(False),
            "cross_workspace_flag_on": cross_workspace(True),
        },
        indent=2,
    )
)
