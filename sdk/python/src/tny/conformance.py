"""Secret-safe conformance report construction for SDK release lanes."""
from __future__ import annotations

import hashlib
import json
import os
import platform as platform_module
from dataclasses import asdict
from pathlib import Path
from typing import Literal, Mapping, TypedDict

from ._binding import Library

SCENARIOS = (
    "success_two_turns",
    "permission_allow_and_stale_reject",
    "permission_deny",
    "cancel_and_drain",
    "auth_error",
    "unknown_future_event",
    "ownership_and_misuse",
    "slow_consumer_backpressure",
)
ScenarioStatus = Literal["pass", "fail", "unsupported", "not_run"]
ReasonCode = Literal[
    "assertions_verified",
    "assertion_failed",
    "capability_unavailable",
    "fixture_unavailable",
    "platform_unavailable",
    "not_executed",
]

ALLOWED_REASONS: dict[str, frozenset[str]] = {
    "pass": frozenset({"assertions_verified"}),
    "fail": frozenset({"assertion_failed"}),
    "unsupported": frozenset({"capability_unavailable"}),
    "not_run": frozenset({
        "fixture_unavailable", "platform_unavailable", "not_executed"
    }),
}


class ScenarioResult(TypedDict):
    status: ScenarioStatus
    reason: ReasonCode


def build_conformance_report(
    artifact: str | os.PathLike[str],
    *,
    library: Library,
    scenarios: Mapping[str, ScenarioResult],
) -> dict[str, object]:
    """Build the v1 report; unknown/missing scenario IDs are rejected."""
    if set(scenarios) != set(SCENARIOS):
        raise ValueError("conformance report requires exactly the v1 scenario IDs")
    artifact_path = Path(artifact)
    digest = hashlib.sha256(artifact_path.read_bytes()).hexdigest()
    normalized: dict[str, ScenarioResult] = {}
    for scenario_id in SCENARIOS:
        result = scenarios[scenario_id]
        if result["status"] not in {"pass", "fail", "unsupported", "not_run"}:
            raise ValueError("invalid conformance status")
        if result["reason"] not in ALLOWED_REASONS[result["status"]]:
            raise ValueError("invalid conformance reason code")
        normalized[scenario_id] = {
            "status": result["status"],
            "reason": result["reason"],
        }
    from . import __version__

    capabilities = library.capabilities
    if capabilities is None:
        raise ValueError("a runtime capability snapshot is required")
    capability_json = {
        key: value.decode("utf-8", "strict") if isinstance(value, bytes) else value
        for key, value in asdict(capabilities).items()
    }
    return {
        "abi_version": f"{library.abi_major}.{library.abi_minor}",
        "library_version": library.version.decode("utf-8", "strict"),
        "sdk": "python",
        "sdk_version": __version__,
        "platform": platform_module.platform(),
        "artifact_sha256": digest,
        "capabilities": capability_json,
        "scenarios": normalized,
    }


def write_conformance_report(report: Mapping[str, object], destination: str | os.PathLike[str]) -> None:
    """Write deterministic JSON without serializing ambient environment state."""
    Path(destination).write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


__all__ = (
    "ALLOWED_REASONS", "ReasonCode", "SCENARIOS", "ScenarioResult",
    "ScenarioStatus",
    "build_conformance_report", "write_conformance_report",
)
