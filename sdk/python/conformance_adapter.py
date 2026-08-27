#!/usr/bin/env python3
"""Protocol-v1 adapter executing the real Python SDK and shared fixtures."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, cast

ROOT = Path(__file__).resolve().parents[2]
USE_INSTALLED = os.environ.get("TNY_CONFORMANCE_USE_INSTALLED") == "1"
if not USE_INSTALLED:
    sys.path.insert(0, str(ROOT / "sdk/python/src"))

import tny  # noqa: E402
from tny.errors import BadStateError  # noqa: E402

CONTRACT_PATH = ROOT / "sdk/conformance/v1.json"
CONTRACT = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
MOCK = ROOT / "tests/integration/mock_openai.py"
STOP_REASONS = {
    0: "done", 1: "interrupted", 2: "denied", 3: "step_limit", 4: "error",
}
STEER_TEXT = next(
    scenario["rejected_text"] for scenario in CONTRACT["scenarios"]
    if scenario["id"] == "resume_and_steer_rejection"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def execution(identifier: str, command: list[str], *, env: dict[str, str] | None = None) -> dict[str, object]:
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=180,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError("required execution failed")
    return {"id": identifier, "exit_code": 0}


def free_port() -> int:
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    port = int(listener.getsockname()[1])
    listener.close()
    return port


def start_mock(**extra: str) -> tuple[subprocess.Popen[bytes], str]:
    port = free_port()
    process = subprocess.Popen(
        [sys.executable, str(MOCK), str(port)],
        cwd=ROOT,
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses", **extra),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    assert process.stdout is not None
    if b"ready" not in process.stdout.readline():
        raise RuntimeError("strict mock did not become ready")
    return process, f"http://127.0.0.1:{port}/v1"


def stop_mock(process: subprocess.Popen[bytes]) -> None:
    process.terminate()
    process.wait(timeout=5)
    if process.stdout is not None:
        process.stdout.close()


def record(event: tny.AnyEvent) -> dict[str, object]:
    value: dict[str, object] = {
        "type": event.type,
        "sequence": event.sequence,
        "timestamp_ms": event.timestamp_ms,
        "provider": event.provider.decode("utf-8", "strict"),
        "session_id": event.session_id.decode("utf-8", "strict"),
        "turn_id": event.turn_id.decode("utf-8", "strict"),
    }
    if isinstance(event, tny.TurnEndEvent):
        value["stop_reason"] = STOP_REASONS[event.stop_reason]
    if isinstance(event, tny.SteerRejectedEvent):
        value["text"] = event.text.decode("utf-8", "strict")
    return value


def collect(
    session: tny.Session,
    prompt: str,
    *,
    decision: tny.PermissionDecision | None = None,
) -> tuple[list[tny.AnyEvent], list[dict[str, object]]]:
    session.send(prompt)
    events: list[tny.AnyEvent] = []
    transcript: list[dict[str, object]] = []
    for event in session.events():
        events.append(event)
        transcript.append(record(event))
        if isinstance(event, tny.PermissionRequestEvent) and decision is not None:
            try:
                session.respond_permission(b"stale-request", decision)
            except BadStateError:
                pass
            else:
                raise AssertionError("stale permission id was accepted")
            session.respond_permission(event, decision)
            try:
                session.respond_permission(event, decision)
            except BadStateError:
                pass
            else:
                raise AssertionError("duplicate permission id was accepted")
    return events, transcript


def python_steer_resume_scenario(
    artifact: Path, secret: str
) -> list[dict[str, object]]:
    library = tny.Library(artifact)
    with tempfile.TemporaryDirectory() as root_value:
        root = Path(root_value)
        workspace = root / "workspace"
        state = root / "state"
        workspace.mkdir()

        process, url = start_mock(MOCK_SLOW_MS="5000")
        try:
            config = tny.RuntimeConfig(
                workspace=workspace,
                state_dir=state,
                persistence=True,
                base_url=url,
                api_key=secret,
            )
            with tny.Runtime(config, library=library) as runtime:
                with runtime.create_session() as session:
                    steered_session_id = session.id
                    session.send("steer then cancel")
                    session.steer(STEER_TEXT)
                    cancel_failures: list[BaseException] = []

                    def cancel_steered_turn() -> None:
                        time.sleep(0.05)
                        try:
                            session.cancel()
                        except BaseException as error:
                            cancel_failures.append(error)

                    thread = threading.Thread(target=cancel_steered_turn)
                    thread.start()
                    interrupted_events = list(session.events())
                    thread.join(timeout=2)
                    assert not thread.is_alive() and not cancel_failures
                    assert isinstance(interrupted_events[-2], tny.SteerRejectedEvent)
                    assert interrupted_events[-2].text == STEER_TEXT.encode()
                    assert isinstance(interrupted_events[-1], tny.TurnEndEvent)
                    assert interrupted_events[-1].stop_reason == 1
                    interrupted_trace = [record(event) for event in interrupted_events]
        finally:
            stop_mock(process)

        process, url = start_mock()
        try:
            resumed_config = tny.RuntimeConfig(
                workspace=workspace,
                state_dir=state,
                persistence=True,
                base_url=url,
                api_key=secret,
            )
            with tny.Runtime(resumed_config, library=library) as resumed_runtime:
                with resumed_runtime.open_session(steered_session_id) as resumed:
                    assert resumed.id == steered_session_id
                    resumed_events, resumed_trace = collect(resumed, "resumed turn")
                    assert any(
                        isinstance(event, tny.TextDeltaEvent)
                        for event in resumed_events
                    )
                    assert isinstance(resumed_events[-1], tny.TurnEndEvent)
                    assert resumed_events[-1].stop_reason == 0
        finally:
            stop_mock(process)
    return interrupted_trace + resumed_trace


def execute_steer_resume_probe(
    artifact: Path, secret: str
) -> tuple[list[dict[str, object]], dict[str, object]]:
    completed = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), "--steer-resume-probe"],
        cwd=ROOT,
        input=json.dumps({"artifact": str(artifact), "secret": secret}) + "\n",
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError("Python steer/resume probe failed")
    return json.loads(completed.stdout), {
        "id": "python_steer_resume_probe", "exit_code": completed.returncode,
    }


def python_live_scenarios(
    artifact: Path, secret: str
) -> tuple[tny.Library, tny.Capabilities, dict[str, list[dict[str, object]]]]:
    library = tny.Library(artifact)
    traces: dict[str, list[dict[str, object]]] = {}
    with tempfile.TemporaryDirectory() as root_value:
        root = Path(root_value)
        workspace = root / "workspace"
        state = root / "state"
        workspace.mkdir()
        for name in ("a.txt", "b.txt", "c.txt"):
            (workspace / name).write_text("x\n", encoding="utf-8")

        process, url = start_mock()
        try:
            config = tny.RuntimeConfig(
                workspace=workspace,
                state_dir=state,
                persistence=True,
                base_url=url,
                api_key=secret,
            )
            with tny.Runtime(config, library=library) as runtime:
                snapshot = runtime.capabilities
                with runtime.create_session() as session:
                    first_events, first = collect(session, "list files in .")
                    second_events, second = collect(session, "again \N{SNOWMAN}")
                    traces["success_two_turns"] = first + second
                    terminals = [
                        event
                        for event in first_events + second_events
                        if isinstance(event, tny.TurnEndEvent)
                    ]
                    assert len(terminals) == 2
                    assert all(event.stop_reason == 0 for event in terminals)
                    copied_provider = first_events[0].provider
                assert copied_provider == b"openai"

        finally:
            stop_mock(process)

        for scenario, decision in (
            ("permission_allow_and_stale_reject", tny.PermissionDecision.ALLOW),
            ("permission_deny", tny.PermissionDecision.DENY),
        ):
            permission_file = workspace / "permission.txt"
            if permission_file.exists():
                permission_file.unlink()
            process, url = start_mock(MOCK_SENSITIVE="1")
            try:
                config = tny.RuntimeConfig(
                    workspace=workspace,
                    state_dir=state,
                    base_url=url,
                    api_key=secret,
                )
                with tny.Runtime(config, library=library) as runtime:
                    with runtime.create_session() as session:
                        events, traces[scenario] = collect(
                            session, "write permission.txt", decision=decision
                        )
                        terminals = [
                            event
                            for event in events
                            if isinstance(event, tny.TurnEndEvent)
                        ]
                        assert len(terminals) == 1
                        assert terminals[0].stop_reason == (
                            0 if decision == tny.PermissionDecision.ALLOW else 2
                        )
                assert permission_file.exists() == (
                    decision == tny.PermissionDecision.ALLOW
                )
            finally:
                stop_mock(process)

        process, url = start_mock(MOCK_SLOW_MS="5000")
        try:
            config = tny.RuntimeConfig(
                workspace=workspace, state_dir=state, base_url=url, api_key=secret
            )
            with tny.Runtime(config, library=library) as runtime:
                with runtime.create_session() as session:
                    session.send("cancel")
                    cancel_failures: list[BaseException] = []

                    def cancel() -> None:
                        time.sleep(0.05)
                        try:
                            session.cancel()
                            session.cancel()
                            session.cancel()
                        except BaseException as error:
                            cancel_failures.append(error)

                    thread = threading.Thread(target=cancel)
                    thread.start()
                    events = list(session.events())
                    thread.join(timeout=2)
                    assert not thread.is_alive() and not cancel_failures
                    traces["cancel_and_drain"] = [record(event) for event in events]
                    terminals = [
                        event for event in events if isinstance(event, tny.TurnEndEvent)
                    ]
                    assert len(terminals) == 1 and terminals[0].stop_reason == 1
                    try:
                        session.next_event(0)
                    except StopIteration:
                        pass
                    else:
                        raise AssertionError("cancelled stream did not drain")
        finally:
            stop_mock(process)

        process, url = start_mock(
            MOCK_HTTP_STATUS="401", MOCK_ERROR_SECRET="provider-body-hidden"
        )
        try:
            config = tny.RuntimeConfig(
                workspace=workspace, state_dir=state, base_url=url, api_key=secret
            )
            with tny.Runtime(config, library=library) as runtime:
                with runtime.create_session() as session:
                    events, traces["auth_error"] = collect(session, "auth")
                    errors = [
                        event for event in events if isinstance(event, tny.ErrorEvent)
                    ]
                    terminals = [
                        event for event in events if isinstance(event, tny.TurnEndEvent)
                    ]
                    assert [event.error_code for event in errors] == [-6]
                    assert len(terminals) == 1 and terminals[0].stop_reason == 4
                    assert all(secret.encode() not in event.text for event in errors)
        finally:
            stop_mock(process)

    return library, snapshot, traces


def shared_reference(
    request: dict[str, object]
) -> tuple[dict[str, object], list[dict[str, object]]]:
    completed = subprocess.run(
        [sys.executable, str(ROOT / "sdk/conformance/adapters/c_reference.py")],
        cwd=ROOT,
        input=json.dumps(request, sort_keys=True) + "\n",
        text=True,
        capture_output=True,
        timeout=180,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError("shared reference adapter failed")
    response = json.loads(completed.stdout)
    executions = [
        {"id": "shared_" + item["id"], "exit_code": item["exit_code"]}
        for item in response["executions"]
    ]
    return response, executions


def main() -> int:
    try:
        request = json.load(sys.stdin)
        contract_bytes = CONTRACT_PATH.read_bytes()
        if request["adapter_protocol_version"] != 1 or request["conformance_version"] != 1:
            raise ValueError("protocol version mismatch")
        if request["contract_sha256"] != hashlib.sha256(contract_bytes).hexdigest():
            raise ValueError("contract hash mismatch")
        requested_artifact = Path(request["artifact"]["path"]).resolve(strict=True)
        if sha256_file(requested_artifact) != request["artifact"]["sha256"]:
            raise ValueError("artifact hash mismatch")
        secret = request["secret_sentinel"]

        if USE_INSTALLED:
            installed_library = tny.Library()
            native_artifact = Path(os.fspath(installed_library.path)).resolve(
                strict=True
            )
            if ".libs" not in native_artifact.parts:
                raise ValueError("installed package has no bundled library")
            artifact_kind = "wheel"
        else:
            native_artifact = requested_artifact
            artifact_kind = "shared"

        if USE_INSTALLED:
            installed_env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
            installed_env.pop("PYTHONPATH", None)
            executions = [
                execution(
                    "python_installed_package_smoke",
                    [
                        sys.executable,
                        "-c",
                        (
                            "import os,tny; p=os.fspath(tny.Library().path); "
                            "assert '.libs' in p; assert tny.__version__"
                        ),
                    ],
                    env=installed_env,
                )
            ]
            sdk_execution = "python_installed_package_smoke"
        else:
            test_env = dict(
                os.environ,
                PYTHONPATH=str(ROOT / "sdk/python/src"),
                TNY_TEST_LIBRARY=str(native_artifact),
                PYTHONDONTWRITEBYTECODE="1",
            )
            executions = [
                execution(
                    "python_sdk_unit_suite",
                    [
                        sys.executable,
                        "-m",
                        "unittest",
                        "discover",
                        "-s",
                        "sdk/python/tests",
                        "-p",
                        "test_sdk.py",
                        "-q",
                    ],
                    env=test_env,
                )
            ]
            sdk_execution = "python_sdk_unit_suite"
        library, snapshot, traces = python_live_scenarios(native_artifact, secret)
        executions.append({"id": "python_live_scenarios", "exit_code": 0})
        traces["resume_and_steer_rejection"], steer_execution = (
            execute_steer_resume_probe(native_artifact, secret)
        )
        executions.append(steer_execution)

        unknown = tny.decode_unknown_event_fixture(
            kind=65535,
            schema_version=1,
            sequence=1,
            timestamp_ms=1,
            provider=b"fixture",
            session_id=b"fixture-session",
            turn_id=b"fixture-turn",
            payload={"future_field": b"future-value"},
        )
        assert unknown.kind == 65535
        assert unknown.payload["future_field"] == b"future-value"
        assert all(
            not isinstance(unknown, event_class)
            for _name, event_class in tny.EVENT_TYPES_BY_KIND.values()
        )
        traces["unknown_future_event"] = [record(unknown)]
        executions.append({"id": "python_unknown_decoder", "exit_code": 0})

        native_request = dict(request)
        native_request["artifact"] = {
            "path": str(native_artifact),
            "sha256": sha256_file(native_artifact),
        }
        reference, shared_executions = shared_reference(native_request)
        executions.extend(shared_executions)
        reference_results = cast(
            list[dict[str, Any]], reference["scenarios"]
        )
        reference_scenarios = {
            result["id"]: result for result in reference_results
        }
        traces["slow_consumer_backpressure"] = reference_scenarios[
            "slow_consumer_backpressure"
        ]["events"]
        traces["network_split_boundaries"] = []

        capabilities = {
            "native_openai": bool(snapshot.provider_available_mask & 1),
            "permissions": True,
            "cancellation": snapshot.cross_thread_native_cancel,
            "persistence": bool(snapshot.feature_available_mask & 2),
            "steering": True,
            "unknown_event_preservation": True,
            "bounded_event_queue": snapshot.event_queue_max > snapshot.event_reserved,
        }
        raw_snapshot = {
            "abi_version": snapshot.abi_version,
            "provider_available_mask": snapshot.provider_available_mask,
            "feature_available_mask": snapshot.feature_available_mask,
            "cancel_model": snapshot.cancel_model,
            "event_queue_max": snapshot.event_queue_max,
            "event_reserved": snapshot.event_reserved,
            "transport": snapshot.transport.decode("utf-8", "strict"),
            "linkage": snapshot.linkage.decode("utf-8", "strict"),
        }
        evidence = {
            "success_two_turns": ["python_live_scenarios", sdk_execution],
            "resume_and_steer_rejection": [
                "python_steer_resume_probe"
            ],
            "permission_allow_and_stale_reject": [
                "python_live_scenarios", sdk_execution
            ],
            "permission_deny": ["python_live_scenarios", sdk_execution],
            "cancel_and_drain": ["python_live_scenarios", sdk_execution],
            "auth_error": ["python_live_scenarios", sdk_execution],
            "unknown_future_event": [
                "python_unknown_decoder", sdk_execution
            ],
            "ownership_and_misuse": [
                sdk_execution, "shared_live_misuse_probe"
            ],
            "slow_consumer_backpressure": ["shared_backpressure_fixture"],
            "network_split_boundaries": ["shared_network_split_fixture"],
        }
        scenarios = [
            {
                "id": scenario["id"],
                "status": "pass",
                "assertions": scenario["assertions"],
                "evidence": evidence[scenario["id"]],
                "events": traces.get(scenario["id"], []),
            }
            for scenario in CONTRACT["scenarios"]
        ]
        response = {
            "conformance_version": 1,
            "adapter_protocol_version": 1,
            "adapter": "python-cffi-v1",
            "sdk": "tny-python",
            "sdk_version": tny.__version__,
            "abi_version": f"{library.abi_major}.{library.abi_minor}",
            "library_version": library.version.decode("utf-8", "strict"),
            "platform": {
                "os": snapshot.platform_family.decode("utf-8", "strict"),
                "arch": snapshot.architecture.decode("utf-8", "strict"),
            },
            "transport": snapshot.transport.decode("utf-8", "strict"),
            "artifact": {
                "sha256": request["artifact"]["sha256"],
                "kind": artifact_kind,
            },
            "capabilities": capabilities,
            "capability_snapshot": raw_snapshot,
            "executions": executions,
            "scenarios": scenarios,
        }
        json.dump(response, sys.stdout, sort_keys=True)
        return 0
    except BaseException:
        print("python conformance adapter failed", file=sys.stderr)
        return 2


if __name__ == "__main__":
    if "--steer-resume-probe" in sys.argv:
        probe = json.load(sys.stdin)
        json.dump(
            python_steer_resume_scenario(Path(probe["artifact"]), probe["secret"]),
            sys.stdout,
            sort_keys=True,
        )
    else:
        raise SystemExit(main())
