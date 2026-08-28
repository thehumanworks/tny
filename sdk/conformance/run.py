#!/usr/bin/env python3
"""Execute one SDK adapter and emit release-blocking conformance evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

from validate import ConformanceError, sha256_file, validate_report

ROOT = Path(__file__).resolve().parents[2]
SAFE_ADAPTER_STAGES = frozenset(
    {
        "fixture-probes",
        "lifecycle-probe",
        "live-abi-probe",
        "live-scenarios",
        "package-smoke",
        "ownership-probe",
        "steer-resume-probe",
        "decoder-probe",
        "unknown-event-probe",
        "report",
    }
)


def safe_stage_marker(stderr: str) -> str | None:
    result = None
    for line in stderr.splitlines():
        fields = line.split(" ", 2)
        if len(fields) != 3 or fields[0] != "conformance-stage:":
            continue
        stage, state = fields[1:]
        if stage not in SAFE_ADAPTER_STAGES:
            continue
        if state == "start":
            result = f"conformance-stage: {stage} start"
        elif state.startswith("error-"):
            result = f"conformance-stage: {stage} error"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description="run a libtny conformance adapter (command follows --)"
    )
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument(
        "--contract", type=Path, default=ROOT / "sdk/conformance/v1.json"
    )
    parser.add_argument("--report", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("an adapter command is required after --")

    artifact = args.artifact.resolve()
    contract_path = args.contract.resolve()
    if not artifact.is_file():
        parser.error(f"artifact does not exist: {artifact}")
    contract_bytes = contract_path.read_bytes()
    contract = json.loads(contract_bytes)
    artifact_sha = sha256_file(artifact)
    sentinel = (
        "tny-conformance-secret-"
        + hashlib.sha256(contract_bytes + artifact_sha.encode("ascii")).hexdigest()[:20]
    )
    request = {
        "adapter_protocol_version": contract["adapter_protocol_version"],
        "conformance_version": contract["conformance_version"],
        "contract_sha256": hashlib.sha256(contract_bytes).hexdigest(),
        "artifact": {"path": str(artifact), "sha256": artifact_sha},
        "secret_sentinel": sentinel,
    }
    try:
        completed = subprocess.run(
            command,
            input=json.dumps(request, sort_keys=True) + "\n",
            text=True,
            capture_output=True,
            timeout=args.timeout,
            check=False,
            cwd=ROOT,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(
            f"conformance: adapter could not complete: {type(error).__name__}",
            file=sys.stderr,
        )
        return 2
    if completed.returncode != 0:
        # Adapter diagnostics may themselves contain caller credentials. The
        # release runner reports only the code and an allowlisted stage marker;
        # it never reflects arbitrary subprocess IO.
        marker = safe_stage_marker(completed.stderr)
        if marker:
            print(marker, file=sys.stderr)
        print(f"conformance: adapter exited {completed.returncode}", file=sys.stderr)
        return 2
    try:
        response = json.loads(completed.stdout)
        report = validate_report(response, contract, artifact, sentinel)
    except (json.JSONDecodeError, ConformanceError, ValueError) as error:
        print(f"conformance: rejected: {error}", file=sys.stderr)
        return 1

    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(encoded, encoding="utf-8")
    else:
        sys.stdout.write(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
