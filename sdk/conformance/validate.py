#!/usr/bin/env python3
"""Validation shared by the executable conformance runner and its tests."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any, Mapping


class ConformanceError(ValueError):
    """A report cannot be used as release evidence."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _need(condition: bool, message: str) -> None:
    if not condition:
        raise ConformanceError(message)


def _mapping(value: Any, name: str) -> Mapping[str, Any]:
    _need(isinstance(value, dict), f"{name} must be an object")
    return value


def _version(value: Any, name: str) -> tuple[int, int]:
    _need(isinstance(value, str), f"{name} must be a major.minor string")
    parts = value.split(".")
    _need(
        len(parts) == 2 and all(part.isdigit() for part in parts),
        f"{name} must be a major.minor string",
    )
    return int(parts[0]), int(parts[1])


def _scan_secrets(
    value: Any, forbidden: set[str], sentinel: str, path: str = "report"
) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = str(key).lower()
            _need(
                normalized not in forbidden,
                f"{path} contains forbidden field {normalized}",
            )
            _scan_secrets(child, forbidden, sentinel, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _scan_secrets(child, forbidden, sentinel, f"{path}[{index}]")
    elif isinstance(value, str):
        lowered = value.lower()
        _need(sentinel not in value, f"{path} contains the secret sentinel")
        _need(
            "authorization: bearer " not in lowered,
            f"{path} contains an authorization value",
        )


def _validate_capabilities(
    report: Mapping[str, Any], contract: Mapping[str, Any]
) -> None:
    capabilities = _mapping(report["capabilities"], "capabilities")
    expected = set(contract["required_capabilities"])
    _need(
        set(capabilities) == expected,
        "capabilities must contain exactly the v1 capability keys",
    )
    _need(
        all(type(value) is bool for value in capabilities.values()),
        "capabilities must be booleans",
    )

    snapshot = _mapping(report["capability_snapshot"], "capability_snapshot")
    integer_fields = (
        "abi_version",
        "provider_available_mask",
        "feature_available_mask",
        "cancel_model",
        "event_queue_max",
        "event_reserved",
    )
    for field in integer_fields:
        _need(
            type(snapshot.get(field)) is int and snapshot[field] >= 0,
            f"capability_snapshot.{field} must be a non-negative integer",
        )
    for field in ("transport", "linkage"):
        _need(
            isinstance(snapshot.get(field), str) and snapshot[field],
            f"capability_snapshot.{field} must be a non-empty string",
        )

    raw_abi = snapshot["abi_version"]
    _need(
        f"{raw_abi >> 16}.{raw_abi & 0xFFFF}" == report["abi_version"],
        "ABI version does not match the raw capability snapshot",
    )
    _need(
        snapshot["transport"] == report["transport"],
        "transport does not match the raw capability snapshot",
    )
    artifact = _mapping(report["artifact"], "artifact")
    if artifact.get("kind") == "shared":
        _need(
            snapshot["linkage"] == "shared",
            "shared artifact contradicts capability linkage",
        )
    if capabilities["native_openai"]:
        _need(
            snapshot["provider_available_mask"] & 1 == 1,
            "native_openai claim is absent from provider_available_mask",
        )
    if capabilities["persistence"]:
        _need(
            snapshot["feature_available_mask"] & 2 == 2,
            "persistence claim is absent from feature_available_mask",
        )
    if capabilities["cancellation"]:
        _need(
            snapshot["feature_available_mask"] & 128 == 128
            and snapshot["cancel_model"] == 2,
            "cancellation claim contradicts the ABI capability snapshot",
        )
    if capabilities["bounded_event_queue"]:
        _need(
            snapshot["event_queue_max"] > snapshot["event_reserved"] >= 2,
            "bounded_event_queue claim lacks reserved terminal capacity",
        )


def _validate_events(result: Mapping[str, Any], scenario: Mapping[str, Any]) -> None:
    events = result.get("events")
    _need(isinstance(events, list), f"{scenario['id']}: events must be an array")
    for event in events:
        _need(isinstance(event, dict), f"{scenario['id']}: event must be an object")
        _need(
            isinstance(event.get("type"), str),
            f"{scenario['id']}: event type is required",
        )
        _need(
            type(event.get("sequence")) is int and event["sequence"] >= 0,
            f"{scenario['id']}: event sequence is required",
        )
        _need(
            type(event.get("timestamp_ms")) is int and event["timestamp_ms"] >= 0,
            f"{scenario['id']}: event timestamp_ms is required",
        )

    actual_types = [event["type"] for event in events]
    cursor = 0
    for expected in scenario["events"]:
        try:
            cursor = actual_types.index(expected, cursor) + 1
        except ValueError as error:
            raise ConformanceError(
                f"{scenario['id']}: missing ordered event {expected}"
            ) from error

    sequences = [event["sequence"] for event in events]
    allow_reset = scenario.get("allow_reopen_clock_reset") is True
    _need(
        all(
            left < right or (allow_reset and events[index]["type"] == "turn_end")
            for index, (left, right) in enumerate(zip(sequences, sequences[1:]))
        ),
        f"{scenario['id']}: event sequences are not strictly increasing",
    )
    timestamps = [event["timestamp_ms"] for event in events]
    _need(
        all(
            left <= right or (allow_reset and events[index]["type"] == "turn_end")
            for index, (left, right) in enumerate(zip(timestamps, timestamps[1:]))
        ),
        f"{scenario['id']}: event timestamps are not monotonic",
    )

    terminals = [event for event in events if event["type"] == "turn_end"]
    if "terminal_reasons" in scenario:
        _need(
            [event.get("stop_reason") for event in terminals]
            == scenario["terminal_reasons"],
            f"{scenario['id']}: wrong ordered terminal reasons",
        )
        _need(
            events and events[-1]["type"] == "turn_end",
            f"{scenario['id']}: terminal must be the final event",
        )
    elif "terminal" in scenario:
        _need(
            len(terminals) == scenario["terminal_count"],
            f"{scenario['id']}: wrong terminal count",
        )
        _need(
            all(
                event.get("stop_reason") == scenario["terminal"] for event in terminals
            ),
            f"{scenario['id']}: wrong terminal reason",
        )
        _need(
            events and events[-1]["type"] == "turn_end",
            f"{scenario['id']}: terminal must be the final event",
        )

    if "rejected_text" in scenario:
        rejected = [
            (index, event)
            for index, event in enumerate(events)
            if event["type"] == "steer_rejected"
        ]
        _need(
            len(rejected) == 1,
            f"{scenario['id']}: expected exactly one steer rejection",
        )
        index, event = rejected[0]
        _need(
            event.get("text") == scenario["rejected_text"],
            f"{scenario['id']}: rejected steer text was not preserved",
        )
        _need(
            index + 1 < len(events)
            and events[index + 1]["type"] == "turn_end"
            and events[index + 1].get("stop_reason") == "interrupted",
            f"{scenario['id']}: steer rejection must immediately precede interrupted terminal",
        )

    assertions = set(result["assertions"])
    if "stable_auth_category" in assertions:
        errors = [event for event in events if event["type"] == "error"]
        _need(
            len(errors) == 1 and type(errors[0].get("error_code")) is int,
            f"{scenario['id']}: stable error category is missing",
        )
    if scenario["id"] == "unknown_future_event":
        _need(
            len(events) == 1
            and type(events[0].get("kind")) is int
            and events[0]["kind"] > 13,
            f"{scenario['id']}: unknown numeric kind was not preserved",
        )
        payload = events[0].get("payload")
        _need(
            isinstance(payload, dict) and bool(payload),
            f"{scenario['id']}: unknown payload was not preserved",
        )


def validate_report(
    report: Any, contract: Mapping[str, Any], artifact: Path, sentinel: str
) -> dict[str, Any]:
    """Return a deterministic release report or raise ``ConformanceError``."""
    report = _mapping(report, "report")
    required = set(contract["report_required"])
    _need(
        required <= set(report),
        f"report missing fields: {sorted(required - set(report))}",
    )
    _need(set(report) == required, "report contains fields outside protocol v1")
    _scan_secrets(report, set(contract["forbidden_report_fields"]), sentinel)

    for field in ("conformance_version", "adapter_protocol_version"):
        _need(report[field] == contract[field], f"wrong {field}")
    for field in ("adapter", "sdk", "sdk_version", "library_version", "transport"):
        _need(
            isinstance(report[field], str) and report[field],
            f"{field} must be a non-empty string",
        )
    _need(
        _version(report["abi_version"], "abi_version")
        >= _version(contract["minimum_abi"], "minimum_abi"),
        "artifact ABI is older than the conformance minimum",
    )

    platform = _mapping(report["platform"], "platform")
    _need(
        set(platform) == {"os", "arch"}
        and all(isinstance(value, str) and value for value in platform.values()),
        "platform must contain non-empty os and arch strings",
    )
    artifact_report = _mapping(report["artifact"], "artifact")
    _need(
        set(artifact_report) == {"sha256", "kind"},
        "artifact must contain exactly sha256 and kind",
    )
    _need(
        artifact_report["kind"]
        in {"shared", "static", "wasm", "dll", "addon", "wheel", "package"},
        "unsupported artifact kind",
    )
    _need(
        artifact_report["sha256"] == sha256_file(artifact),
        "artifact SHA-256 does not match the executed artifact",
    )
    _validate_capabilities(report, contract)

    executions = report["executions"]
    _need(
        isinstance(executions, list) and executions,
        "at least one adapter execution is required",
    )
    known_assertions = {
        f"{scenario['id']}:{assertion}"
        for scenario in contract["scenarios"]
        for assertion in scenario["assertions"]
    }
    execution_ids: set[str] = set()
    successful: set[str] = set()
    execution_assertions: dict[str, set[str]] = {}
    for execution in executions:
        _need(
            isinstance(execution, dict)
            and set(execution) == {"id", "exit_code", "assertions"},
            "execution must contain exactly id, exit_code, and assertions",
        )
        _need(
            isinstance(execution["id"], str)
            and execution["id"]
            and execution["id"] not in execution_ids,
            "execution IDs must be unique non-empty strings",
        )
        _need(
            type(execution["exit_code"]) is int,
            "execution exit_code must be an integer",
        )
        claims = execution["assertions"]
        _need(
            isinstance(claims, list)
            and all(isinstance(claim, str) for claim in claims)
            and len(claims) == len(set(claims)),
            f"execution {execution['id']} assertions must be unique strings",
        )
        _need(
            set(claims) <= known_assertions,
            f"execution {execution['id']} claims unknown assertions: "
            f"{sorted(set(claims) - known_assertions)}",
        )
        execution_ids.add(execution["id"])
        execution_assertions[execution["id"]] = set(claims)
        if execution["exit_code"] == 0:
            successful.add(execution["id"])
    failed_executions = sorted(execution_ids - successful)
    _need(not failed_executions, f"adapter executions failed: {failed_executions}")

    capabilities = report["capabilities"]
    results = report["scenarios"]
    _need(isinstance(results, list), "scenarios must be an array")
    by_id: dict[str, Mapping[str, Any]] = {}
    for result in results:
        _need(
            isinstance(result, dict) and isinstance(result.get("id"), str),
            "scenario result requires an id",
        )
        _need(result["id"] not in by_id, f"duplicate scenario {result['id']}")
        by_id[result["id"]] = result
    expected_ids = {scenario["id"] for scenario in contract["scenarios"]}
    _need(
        set(by_id) == expected_ids,
        f"scenario IDs differ from contract: {sorted(set(by_id) ^ expected_ids)}",
    )

    referenced_claims: set[tuple[str, str]] = set()
    for scenario in contract["scenarios"]:
        result = by_id[scenario["id"]]
        status = result.get("status")
        _need(
            status in {"pass", "fail", "unsupported", "not_run"},
            f"{scenario['id']}: invalid status",
        )
        applicable = all(capabilities[name] for name in scenario["requires"])
        if scenario["release_required"] and applicable:
            _need(
                status == "pass",
                f"{scenario['id']}: release-required scenario is {status}",
            )
        elif not applicable:
            _need(
                status in {"pass", "unsupported"},
                f"{scenario['id']}: unavailable capability must be unsupported",
            )
        if status == "pass":
            _need(
                set(result) == {"id", "status", "assertions", "evidence", "events"},
                f"{scenario['id']}: pass result has fields outside protocol v1",
            )
            assertions = result.get("assertions")
            _need(
                isinstance(assertions, list)
                and set(assertions) == set(scenario["assertions"])
                and len(assertions) == len(set(assertions)),
                f"{scenario['id']}: assertion evidence is incomplete",
            )
            evidence = result.get("evidence")
            _need(
                isinstance(evidence, list)
                and evidence
                and all(item in successful for item in evidence),
                f"{scenario['id']}: evidence must reference successful executions",
            )
            for assertion in assertions:
                qualified = f"{scenario['id']}:{assertion}"
                _need(
                    any(qualified in execution_assertions[item] for item in evidence),
                    f"{scenario['id']}: assertion {assertion} has no executable evidence",
                )
            for item in evidence:
                prefix = f"{scenario['id']}:"
                _need(
                    any(
                        claim.startswith(prefix) for claim in execution_assertions[item]
                    ),
                    f"{scenario['id']}: evidence {item} does not probe this scenario",
                )
                referenced_claims.update(
                    (item, claim)
                    for claim in execution_assertions[item]
                    if claim.startswith(prefix)
                )
            _validate_events(result, scenario)
        else:
            _need(
                set(result) == {"id", "status", "reason"},
                f"{scenario['id']}: non-pass result has fields outside protocol v1",
            )
            _need(
                isinstance(result.get("reason"), str) and result["reason"],
                f"{scenario['id']}: non-pass result needs a reason",
            )

    dangling = sorted(
        f"{execution_id}={claim}"
        for execution_id, claims in execution_assertions.items()
        for claim in claims
        if (execution_id, claim) not in referenced_claims
    )
    _need(not dangling, f"execution assertions are not referenced: {dangling}")

    # Normalize volatile native timestamps and opaque session/turn IDs only
    # after validating them. The emitted report records their presence without
    # making identical executions differ byte-for-byte.
    canonical = json.loads(json.dumps(report, sort_keys=True, allow_nan=False))
    for result in canonical["scenarios"]:
        if result["status"] != "pass":
            continue
        normalized_events = []
        for index, event in enumerate(result["events"], 1):
            normalized = {
                "type": event["type"],
                "sequence": index,
                "timestamp_ms": index,
            }
            if "stop_reason" in event:
                normalized["stop_reason"] = event["stop_reason"]
            if event["type"] == "steer_rejected":
                normalized["text"] = event["text"]
            if event["type"] == "error" and "error_code" in event:
                normalized["error_code"] = event["error_code"]
            if event["type"] == "unknown":
                normalized["kind"] = event["kind"]
                normalized["payload"] = event["payload"]
            for field in ("provider", "session_id", "turn_id"):
                if field in event:
                    normalized[f"{field}_present"] = bool(event[field])
            normalized_events.append(normalized)
        result["events"] = normalized_events
    return canonical
