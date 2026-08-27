#!/usr/bin/env python3
"""Self-test adapter for the orchestrator; never release evidence."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
contract = json.loads((ROOT / "sdk/conformance/v1.json").read_text())
request = json.load(sys.stdin)


def events_for(scenario: dict[str, object]) -> list[dict[str, object]]:
    result = []
    terminal_index = 0
    for index, event_type in enumerate(scenario["events"], 1):
        event: dict[str, object] = {
            "type": event_type, "sequence": index, "timestamp_ms": index,
        }
        if event_type == "turn_end":
            if "terminal_reasons" in scenario:
                event["stop_reason"] = scenario["terminal_reasons"][terminal_index]
                terminal_index += 1
            else:
                event["stop_reason"] = scenario["terminal"]
        if event_type == "steer_rejected":
            event["text"] = scenario["rejected_text"]
        if event_type == "error":
            event["error_code"] = -6
        if event_type == "unknown":
            event["kind"] = 65535
            event["payload"] = {"future_field": "future-value"}
        result.append(event)
    return result


response = {
    "conformance_version": 1,
    "adapter_protocol_version": 1,
    "adapter": "orchestrator-self-test",
    "sdk": "fixture",
    "sdk_version": "1.0.0",
    "abi_version": "1.0",
    "library_version": "fixture",
    "platform": {"os": "fixture-os", "arch": "fixture-arch"},
    "transport": "native-http1",
    "artifact": {"sha256": request["artifact"]["sha256"], "kind": "shared"},
    "capabilities": {name: True for name in contract["required_capabilities"]},
    "capability_snapshot": {
        "abi_version": 65536,
        "provider_available_mask": 1,
        "feature_available_mask": 0x87,
        "cancel_model": 2,
        "event_queue_max": 256,
        "event_reserved": 2,
        "transport": "native-http1",
        "linkage": "shared",
    },
    "executions": [{
        "id": "fixture",
        "exit_code": 0,
        "assertions": [
            f"{scenario['id']}:{assertion}"
            for scenario in contract["scenarios"]
            for assertion in scenario["assertions"]
        ],
    }],
    "scenarios": [{
        "id": scenario["id"],
        "status": "pass",
        "assertions": scenario["assertions"],
        "evidence": ["fixture"],
        "events": events_for(scenario),
    } for scenario in contract["scenarios"]],
}

mutation_path = os.environ.get("TNY_CONFORMANCE_MUTATION")
if mutation_path:
    mutation = json.loads(Path(mutation_path).read_text())
    kind = mutation["mutation"]
    if kind == "event_order":
        response["scenarios"][0]["events"].reverse()
    elif kind == "rejected_text":
        response["scenarios"][1]["events"][0]["text"] = "wrong text"
    elif kind == "terminal_reasons":
        for event in response["scenarios"][1]["events"]:
            if event["type"] == "turn_end":
                event["stop_reason"] = "done"
    elif kind == "capability_claim":
        response["capability_snapshot"]["provider_available_mask"] = 0
    elif kind == "artifact_hash":
        response["artifact"]["sha256"] = "0" * 64
    elif kind == "secret_field":
        response["authorization"] = "Bearer " + request["secret_sentinel"]
    elif kind == "missing_scenario":
        response["scenarios"].pop()
    elif kind in {"not_run", "unsupported"}:
        response["scenarios"][0] = {
            "id": response["scenarios"][0]["id"],
            "status": kind,
            "reason": "intentional negative fixture",
        }
    elif kind == "false_assertion_mapping":
        missing = (
            f"{contract['scenarios'][0]['id']}:"
            f"{contract['scenarios'][0]['assertions'][0]}"
        )
        response["executions"][0]["assertions"].remove(missing)
    elif kind == "unsupported_assertion_claim":
        response["executions"][0]["assertions"].append(
            "success_two_turns:not_in_the_contract"
        )
    elif kind == "unreferenced_assertion_claim":
        response["executions"].append({
            "id": "dangling",
            "exit_code": 0,
            "assertions": ["success_two_turns:create_and_open"],
        })
    else:
        raise RuntimeError(f"unknown fixture mutation {kind}")

json.dump(response, sys.stdout, sort_keys=True)
