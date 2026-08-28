#!/usr/bin/env python3
"""Structural guard for the executable shared SDK conformance contract."""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
contract = json.loads((ROOT / "sdk/conformance/v1.json").read_text())
events = json.loads((ROOT / "sdk/schema/events.json").read_text())

assert contract["conformance_version"] == 1
assert contract["adapter_protocol_version"] == 1
assert contract["event_schema_version"] == events["schema_version"]
assert contract["minimum_abi"] == "1.0"

known_events = {event["type"] for event in events["events"]} | {"unknown"}
capabilities = set(contract["required_capabilities"])
scenarios = contract["scenarios"]
ids = [scenario["id"] for scenario in scenarios]
assert len(ids) == len(set(ids)), "duplicate scenario id"
assert {
    "success_two_turns",
    "resume_and_steer_rejection",
    "permission_allow_and_stale_reject",
    "permission_deny",
    "cancel_and_drain",
    "auth_error",
    "unknown_future_event",
    "ownership_and_misuse",
    "slow_consumer_backpressure",
    "network_split_boundaries",
} == set(ids), "missing or unexpected conformance scenario"

for scenario in scenarios:
    assert scenario["release_required"] is True, scenario["id"]
    assert set(scenario["events"]) <= known_events, scenario["id"]
    assert set(scenario["requires"]) <= capabilities, scenario["id"]
    assert scenario["assertions"], scenario["id"]
    assert len(scenario["assertions"]) == len(set(scenario["assertions"])), scenario[
        "id"
    ]
    if "terminal" in scenario:
        assert scenario["terminal_count"] > 0, scenario["id"]
    if "terminal_reasons" in scenario:
        assert scenario["terminal_reasons"], scenario["id"]
        assert "terminal" not in scenario and "terminal_count" not in scenario
    if "rejected_text" in scenario:
        assert scenario["rejected_text"] and "steer_rejected" in scenario["events"]
    if scenario.get("allow_reopen_clock_reset"):
        assert len(scenario.get("terminal_reasons", [])) > 1, scenario["id"]

required_report = set(contract["report_required"])
assert {
    "artifact",
    "capabilities",
    "capability_snapshot",
    "executions",
    "scenarios",
} <= required_report
for forbidden in contract["forbidden_report_fields"]:
    assert forbidden not in required_report

print(
    f"conformance v1: {len(scenarios)} executable scenarios, schema {events['schema_version']}"
)
