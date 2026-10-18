#!/usr/bin/env python3
"""Structural guard for the executable shared SDK conformance contract."""

from __future__ import annotations

import json
from pathlib import Path


def require(condition: object, message: str) -> None:
    if not condition:
        raise ValueError(message)


ROOT = Path(__file__).resolve().parents[2]
contract = json.loads((ROOT / "sdk/conformance/v1.json").read_text())
events = json.loads((ROOT / "sdk/schema/events.json").read_text())

require(contract["conformance_version"] == 1, "unsupported conformance_version")
require(
    contract["adapter_protocol_version"] == 1, "unsupported adapter_protocol_version"
)
require(
    contract["event_schema_version"] == events["schema_version"],
    "event_schema_version mismatch",
)
require(contract["minimum_abi"] == "1.0", "unsupported minimum_abi")

known_events = {event["type"] for event in events["events"]} | {"unknown"}
capabilities = set(contract["required_capabilities"])
scenarios = contract["scenarios"]
ids = [scenario["id"] for scenario in scenarios]
require(len(ids) == len(set(ids)), "duplicate scenario id")
require(
    {
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
    }
    == set(ids),
    "missing or unexpected conformance scenario",
)

for scenario in scenarios:
    name = scenario["id"]
    require(
        scenario["release_required"] is True, f"{name}: release_required must be true"
    )
    require(set(scenario["events"]) <= known_events, f"{name}: unknown event")
    require(set(scenario["requires"]) <= capabilities, f"{name}: unknown capability")
    require(scenario["assertions"], f"{name}: assertions must not be empty")
    require(
        len(scenario["assertions"]) == len(set(scenario["assertions"])),
        f"{name}: duplicate assertion",
    )
    if "terminal" in scenario:
        require(
            scenario["terminal_count"] > 0, f"{name}: terminal_count must be positive"
        )
    if "terminal_reasons" in scenario:
        require(
            scenario["terminal_reasons"], f"{name}: terminal_reasons must not be empty"
        )
        require(
            "terminal" not in scenario and "terminal_count" not in scenario,
            f"{name}: terminal_reasons excludes terminal and terminal_count",
        )
    if "rejected_text" in scenario:
        require(
            scenario["rejected_text"] and "steer_rejected" in scenario["events"],
            f"{name}: rejected_text requires text and a steer_rejected event",
        )
    if scenario.get("allow_reopen_clock_reset"):
        require(
            len(scenario.get("terminal_reasons", [])) > 1,
            f"{name}: clock reset requires multiple terminal_reasons",
        )

required_report = set(contract["report_required"])
require(
    {"artifact", "capabilities", "capability_snapshot", "executions", "scenarios"}
    <= required_report,
    "missing required report field",
)
for forbidden in contract["forbidden_report_fields"]:
    require(forbidden not in required_report, f"forbidden report field: {forbidden}")

print(
    f"conformance v1: {len(scenarios)} executable scenarios, schema {events['schema_version']}"
)
