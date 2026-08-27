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
from typing import Any

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
COMMAND_TIMEOUT = int(os.environ.get("TNY_CONFORMANCE_COMMAND_TIMEOUT", "180"))
STEER_TIMEOUT = int(os.environ.get("TNY_CONFORMANCE_STEER_TIMEOUT", "30"))
if COMMAND_TIMEOUT < 1:
    raise ValueError("TNY_CONFORMANCE_COMMAND_TIMEOUT must be positive")
if STEER_TIMEOUT < 1:
    raise ValueError("TNY_CONFORMANCE_STEER_TIMEOUT must be positive")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def qualified(scenario: str, *assertions: str) -> list[str]:
    return [f"{scenario}:{assertion}" for assertion in assertions]


def execution(identifier: str, command: list[str], *,
              env: dict[str, str] | None = None,
              assertions: list[str] | None = None) -> dict[str, object]:
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
    return {"id": identifier, "exit_code": 0, "assertions": assertions or []}


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
    if isinstance(event, tny.ErrorEvent):
        value["error_code"] = event.error_code
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
        timeout=STEER_TIMEOUT,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError("Python steer/resume probe failed")
    return json.loads(completed.stdout), {
        "id": "python_steer_resume_probe", "exit_code": completed.returncode,
        "assertions": qualified(
            "resume_and_steer_rejection", "rejected_text_preserved",
            "resume_same_session", "teardown_and_reopen"),
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
            ownership_env = dict(
                installed_env, TNY_TEST_LIBRARY=str(native_artifact)
            )
            executions.append(execution(
                "python_installed_ownership_suite",
                [sys.executable, "-m", "unittest", "discover", "-s",
                 "sdk/python/tests", "-p", "test_sdk.py", "-q"],
                env=ownership_env,
                assertions=qualified(
                    "ownership_and_misuse", "inputs_copied",
                    "event_and_error_lifetimes", "double_free_prevention",
                    "wrong_thread_rejected", "invalid_utf8_rejected",
                    "embedded_nul_rejected", "unknown_constants_rejected",
                    "undersized_struct_rejected", "oversized_struct_prefix_safe",
                    "parent_close_releases_children", "repeated_lifecycle"),
            ))
            sdk_execution = "python_installed_ownership_suite"
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
                    assertions=qualified(
                        "ownership_and_misuse", "inputs_copied",
                        "event_and_error_lifetimes", "double_free_prevention",
                        "wrong_thread_rejected", "invalid_utf8_rejected",
                        "embedded_nul_rejected", "unknown_constants_rejected",
                        "undersized_struct_rejected", "oversized_struct_prefix_safe",
                        "parent_close_releases_children", "repeated_lifecycle"),
                )
            ]
            sdk_execution = "python_sdk_unit_suite"
        library, snapshot, traces = python_live_scenarios(native_artifact, secret)
        if library.abi_minor >= 7:
            callback_env = dict(
                installed_env if USE_INSTALLED else test_env,
                TNY_TEST_LIBRARY=str(native_artifact),
            )
            executions.append(execution(
                "python_callback_acceptance",
                [sys.executable, "-m", "unittest", "discover", "-s",
                 "sdk/python/tests", "-p", "test_callbacks.py", "-q"],
                env=callback_env,
            ))
        executions.append({
            "id": "python_live_scenarios", "exit_code": 0,
            "assertions": (
                qualified(
                    "success_two_turns", "create_and_open",
                    "sequence_strictly_increases", "timestamps_monotonic",
                    "provider_session_turn_present", "borrowed_bytes_copied_before_free",
                    "second_turn_same_session") +
                qualified(
                    "permission_allow_and_stale_reject", "parked_before_response",
                    "stale_id_bad_state", "duplicate_id_bad_state") +
                qualified("permission_deny", "denied_tool_not_executed") +
                qualified(
                    "cancel_and_drain", "cancel_idempotent", "exactly_one_terminal",
                    "drained_after_terminal", "cross_thread_wake") +
                qualified(
                    "auth_error", "stable_auth_category", "no_raw_provider_body",
                    "no_credentials")
            ),
        })
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
        unknown_record = record(unknown)
        unknown_record["kind"] = unknown.kind
        unknown_record["payload"] = {
            key: value.decode("utf-8", "strict") if isinstance(value, bytes)
            else str(value)
            for key, value in unknown.payload.items()
        }
        traces["unknown_future_event"] = [unknown_record]
        executions.append({
            "id": "python_unknown_decoder", "exit_code": 0,
            "assertions": qualified(
                "unknown_future_event", "numeric_kind_preserved",
                "payload_preserved", "known_union_not_aliased"),
        })

        executions.extend([
            execution("python_build_c_fixtures", ["make", "debug"]),
            execution("python_network_split_fixture", [
                "./build/tny-test", "-s", "net_suite", "-t",
                "chunked_survives_every_split_boundary", "-e",
            ], assertions=qualified(
                "network_split_boundaries",
                "existing_chunked_fixture_every_split_boundary")),
            execution("python_backpressure_fixture", [
                "./build/tny-test", "-s", "runtime_suite", "-t",
                "runtime_overflow_keeps_error_and_single_terminal", "-e",
            ], assertions=qualified(
                "slow_consumer_backpressure", "memory_bounded",
                "stable_backpressure_category", "terminal_reserved")),
        ])
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
            "success_two_turns": ["python_live_scenarios"],
            "resume_and_steer_rejection": [
                "python_steer_resume_probe"
            ],
            "permission_allow_and_stale_reject": [
                "python_live_scenarios"
            ],
            "permission_deny": ["python_live_scenarios"],
            "cancel_and_drain": ["python_live_scenarios"],
            "auth_error": ["python_live_scenarios"],
            "unknown_future_event": ["python_unknown_decoder"],
            "ownership_and_misuse": [sdk_execution],
            "slow_consumer_backpressure": ["python_backpressure_fixture"],
            "network_split_boundaries": ["python_network_split_fixture"],
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
