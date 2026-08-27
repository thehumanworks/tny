from __future__ import annotations

import asyncio
import gc
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import warnings
import weakref
from dataclasses import fields
from pathlib import Path

import tny

ROOT = Path(__file__).resolve().parents[3]
MOCK = ROOT / "tests" / "integration" / "mock_openai.py"
LIBRARY = Path(
    os.environ.get(
        "TNY_TEST_LIBRARY",
        ROOT
        / "build"
        / "lib"
        / ("libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1"),
    )
)


def free_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class Mock:
    def __init__(self, **environment: str) -> None:
        self.port = free_port()
        self.process = subprocess.Popen(
            [sys.executable, os.fspath(MOCK), str(self.port)],
            env=dict(os.environ, MOCK_EXPECT_WIRE="responses", **environment),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert self.process.stdout is not None
        ready = self.process.stdout.readline()
        if b"ready" not in ready:
            raise RuntimeError("mock did not become ready")

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}/v1"

    def close(self) -> None:
        self.process.terminate()
        self.process.wait(timeout=5)
        if self.process.stdout is not None:
            self.process.stdout.close()
        if self.process.stderr is not None:
            self.process.stderr.close()


class SDKTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.workspace = Path(self.temp.name, "workspace")
        self.state = Path(self.temp.name, "state")
        self.workspace.mkdir()
        for name in ("a.txt", "b.txt", "c.txt"):
            self.workspace.joinpath(name).write_text("x\n")
        self.library = tny.Library(LIBRARY)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def config(self, url: str, **kwargs: object) -> tny.RuntimeConfig:
        return tny.RuntimeConfig(
            workspace=self.workspace,
            state_dir=self.state,
            base_url=url,
            api_key="sdk-test-key-not-real",
            **kwargs,
        )

    def test_metadata_capabilities_and_secret_safe_repr(self) -> None:
        self.assertEqual((self.library.abi_major, self.library.abi_minor), (1, 0))
        with tny.Runtime(
            self.config("https://api.example.invalid/v1"), library=self.library
        ) as runtime:
            self.assertEqual(runtime.config.api_key, b"")
            self.assertFalse(runtime.capabilities.custom_tool_callbacks)
            self.assertTrue(runtime.capabilities.cross_thread_native_cancel)
            self.assertEqual(runtime.capabilities.library_version, self.library.version)
            self.assertTrue(runtime.capabilities.platform_family)
            self.assertTrue(runtime.capabilities.architecture)
        config = self.config("https://user:password@example.invalid/v1")
        rendered = repr(config)
        self.assertNotIn("sdk-test-key", rendered)
        self.assertNotIn("password", rendered)
        with self.assertRaises(tny.InvalidArgumentError) as caught:
            tny.Runtime(config, library=self.library)
        self.assertNotIn("password", str(caught.exception))
        with self.assertRaises(tny.InvalidArgumentError):
            tny.Runtime(
                self.config("https://api.example.invalid/v1", max_steps=1 << 31),
                library=self.library,
            )

    def test_api_key_staging_is_wiped_when_later_input_encoding_fails(self) -> None:
        import tny.runtime as runtime_module

        original_borrowed = runtime_module.borrowed
        captured: list[tuple[object, object]] = []

        def capture(ffi: object, value: object) -> tuple[object, object]:
            buffer, view = original_borrowed(ffi, value)  # type: ignore[arg-type]
            if value == b"SECRET-STAGING":
                captured.append((ffi, buffer))
            return buffer, view

        runtime_module.borrowed = capture  # type: ignore[assignment]
        try:
            with self.assertRaises(UnicodeEncodeError):
                tny.Runtime(
                    tny.RuntimeConfig(
                        workspace=self.workspace,
                        api_key=b"SECRET-STAGING",
                        wire_api="\ud800",
                    ),
                    library=self.library,
                )
        finally:
            runtime_module.borrowed = original_borrowed
        self.assertEqual(len(captured), 1)
        ffi, buffer = captured[0]
        raw = bytes(ffi.buffer(buffer, ffi.sizeof(buffer)))  # type: ignore[attr-defined]
        self.assertEqual(raw, b"\0" * len(raw))

    def test_repeated_create_close_and_owner_thread(self) -> None:
        """Conformance: ownership_and_misuse."""
        config = self.config("https://api.example.invalid/v1")
        for _ in range(20):
            runtime = tny.Runtime(config, library=self.library)
            session = runtime.create_session()
            result: list[type[BaseException]] = []

            def wrong_thread() -> None:
                try:
                    session.steer("not active")
                except BaseException as error:
                    result.append(type(error))

            thread = threading.Thread(target=wrong_thread)
            thread.start()
            thread.join()
            self.assertEqual(result, [tny.BadStateError])
            session.close()
            runtime.close()
        gc.collect()

    def test_abi1_allows_multiple_python_runtimes(self) -> None:
        config = self.config("https://api.example.invalid/v1")
        first = tny.Runtime(config, library=self.library)
        second = tny.Runtime(config, library=self.library)
        try:
            self.assertFalse(first.closed)
            self.assertFalse(second.closed)
            self.assertTrue(first.capabilities.cross_thread_native_cancel)
            self.assertTrue(second.capabilities.cross_thread_native_cancel)
        finally:
            second.close()
            first.close()

    def test_input_sizing_utf8_and_parent_lifecycle_misuse(self) -> None:
        """Conformance: ownership_and_misuse input and parent assertions."""
        config = tny.RuntimeConfig(
            workspace=self.workspace,
            base_url="https://api.example.invalid/v1",
            api_key="not-real",
        )
        runtime = tny.Runtime(config, library=self.library)
        session = runtime.create_session()
        with self.assertRaises(tny.InvalidArgumentError):
            session.send(b"embedded\x00nul")
        with self.assertRaises(tny.InvalidArgumentError):
            session.send(b"\xff")

        ffi = self.library.ffi
        native = self.library.native
        self.assertEqual(ffi.sizeof("tny_runtime_options_v0"), 200)
        self.assertEqual(ffi.sizeof("tny_event_view_v0"), 328)
        self.assertEqual(ffi.sizeof("tny_capabilities_v0"), 240)
        self.assertEqual(ffi.offsetof("tny_runtime_options_v0", "reserved"), 136)
        self.assertEqual(ffi.offsetof("tny_event_view_v0", "provider"), 88)
        self.assertEqual(ffi.offsetof("tny_capabilities_v0", "library_version"), 80)
        options = ffi.new("tny_runtime_options_v0 *")
        options_size = ffi.sizeof("tny_runtime_options_v0")
        self.assertEqual(native.tny_runtime_options_init(options, options_size), 0)
        options.struct_size = 4
        out = ffi.new("tny_runtime **")
        error = ffi.new("tny_error **")
        self.assertEqual(
            native.tny_runtime_create(options, options_size, out, error), -1
        )
        if error[0] != ffi.NULL:
            native.tny_error_free(error[0])

        capabilities = ffi.new("tny_capabilities_v0 *")
        capabilities_size = ffi.sizeof("tny_capabilities_v0")
        self.assertEqual(
            native.tny_capabilities_init(capabilities, capabilities_size), 0
        )
        capabilities.struct_size = 4
        self.assertEqual(
            native.tny_runtime_get_capabilities(
                runtime._handle, capabilities, capabilities_size
            ),
            -1,
        )
        runtime.close()
        self.assertTrue(session.closed)
        self.assertTrue(runtime.closed)
        with self.assertRaises(tny.InvalidArgumentError):
            tny.Runtime(
                tny.RuntimeConfig(
                    workspace=self.workspace,
                    persistence=True,
                    base_url="https://api.example.invalid/v1",
                ),
                library=self.library,
            )

    def test_explicit_abi0_library_is_rejected(self) -> None:
        legacy = (
            ROOT
            / "build"
            / "lib"
            / ("libtny.0.dylib" if sys.platform == "darwin" else "libtny.so.0")
        )
        if not legacy.is_file():
            self.skipTest("explicit ABI0 compatibility artifact is not staged")
        with self.assertRaises(tny.UnsupportedError):
            tny.Library(legacy)

    def test_sync_full_turn_events_are_python_owned_bytes(self) -> None:
        """Conformance: success_two_turns and slow_consumer_backpressure basics."""
        mock = Mock()
        try:
            with tny.Runtime(self.config(mock.url), library=self.library) as runtime:
                with runtime.create_session() as session:
                    events = list(session.run("list files in ."))
                    events.extend(session.run("again \N{SNOWMAN}"))
                    del session
                text = b"".join(
                    event.text
                    for event in events
                    if isinstance(event, tny.TextDeltaEvent)
                )
                self.assertIn(b"MOCK-OK", text)
                self.assertTrue(any(isinstance(e, tny.TurnEndEvent) for e in events))
                terminals = [e for e in events if isinstance(e, tny.TurnEndEvent)]
                self.assertEqual(len(terminals), 2)
                self.assertTrue(all(e.stop_reason == 0 for e in terminals))
                self.assertTrue(all(isinstance(e.provider, bytes) for e in events))
                sequences = [e.sequence for e in events]
                self.assertEqual(sequences, sorted(set(sequences)))
        finally:
            mock.close()

    def test_abandoned_sync_and_async_turns_cancel_and_drain(self) -> None:
        mock = Mock(MOCK_SLOW_MS="100")
        try:
            with tny.Runtime(self.config(mock.url), library=self.library) as runtime:
                with runtime.create_session() as session:
                    stream = session.run("first sync turn")
                    next(stream)
                    stream.close()
                    second = list(session.run("second sync turn"))
                    self.assertTrue(
                        any(isinstance(event, tny.TurnEndEvent) for event in second)
                    )

            async def async_case() -> None:
                async with tny.AsyncRuntime(
                    self.config(mock.url), library=self.library
                ) as runtime:
                    async with await runtime.create_session() as session:
                        stream = session.run("first async turn")
                        await anext(stream)
                        await stream.aclose()
                        second = [
                            event async for event in session.run("second async turn")
                        ]
                        self.assertTrue(
                            any(isinstance(event, tny.TurnEndEvent) for event in second)
                        )

            asyncio.run(async_case())
        finally:
            mock.close()

    def test_permission_and_stale_response(self) -> None:
        """Conformance: permission_deny and stale permission rejection."""
        mock = Mock(MOCK_SENSITIVE="1")
        try:
            with tny.Runtime(self.config(mock.url), library=self.library) as runtime:
                with runtime.create_session() as session:
                    session.send("list files in .")
                    saw_permission = False
                    events = []
                    for event in session.events():
                        events.append(event)
                        if isinstance(event, tny.PermissionRequestEvent):
                            saw_permission = True
                            session.respond_permission(
                                event, tny.PermissionDecision.DENY
                            )
                            with self.assertRaises(tny.BadStateError):
                                session.respond_permission(
                                    event, tny.PermissionDecision.DENY
                                )
                    self.assertTrue(saw_permission)
                    terminals = [
                        event for event in events if isinstance(event, tny.TurnEndEvent)
                    ]
                    self.assertEqual(len(terminals), 1)
                    self.assertEqual(terminals[0].stop_reason, 2)
        finally:
            mock.close()

    def test_permission_allow_and_stale_reject(self) -> None:
        """Conformance: permission_allow_and_stale_reject."""
        mock = Mock(MOCK_SENSITIVE="1")
        try:
            with (
                tny.Runtime(self.config(mock.url), library=self.library) as runtime,
                runtime.create_session() as session,
            ):
                session.send("list files in .")
                saw_permission = False
                events = []
                for event in session.events():
                    events.append(event)
                    if isinstance(event, tny.PermissionRequestEvent):
                        saw_permission = True
                        session.respond_permission(event, tny.PermissionDecision.ALLOW)
                        with self.assertRaises(tny.BadStateError):
                            session.respond_permission(
                                event, tny.PermissionDecision.ALLOW
                            )
                self.assertTrue(saw_permission)
                self.assertTrue(self.workspace.joinpath("permission.txt").exists())
                self.assertEqual(
                    len([e for e in events if isinstance(e, tny.TurnEndEvent)]), 1
                )
        finally:
            mock.close()

    def test_thread_safe_cancellation_request(self) -> None:
        """Conformance: cancel_and_drain."""
        mock = Mock()
        try:
            token = tny.CancellationToken()
            with tny.Runtime(self.config(mock.url), library=self.library) as runtime:
                with runtime.create_session() as session:
                    session.send("cancel this turn")
                    cancel_errors: list[BaseException] = []

                    def cancel_from_thread() -> None:
                        try:
                            token.cancel()
                            session.cancel()
                        except BaseException as error:
                            cancel_errors.append(error)

                    thread = threading.Thread(target=cancel_from_thread)
                    thread.start()
                    thread.join()
                    self.assertEqual(cancel_errors, [])
                    session.cancel()
                    events = list(session.events(cancellation=token))
                    terminal = [e for e in events if isinstance(e, tny.TurnEndEvent)]
                    self.assertEqual(len(terminal), 1)
                    self.assertEqual(terminal[0].stop_reason, 1)
                    with self.assertRaises(StopIteration):
                        session.next_event(0)
        finally:
            mock.close()

    def test_async_full_turn(self) -> None:
        mock = Mock()

        async def run() -> None:
            async with tny.AsyncRuntime(
                self.config(mock.url), library=self.library
            ) as runtime:
                async with await runtime.create_session() as session:
                    events = [event async for event in session.run("list files in .")]
                    text = b"".join(
                        event.text
                        for event in events
                        if isinstance(event, tny.TextDeltaEvent)
                    )
                    self.assertIn(b"MOCK-OK", text)

        try:
            asyncio.run(run())
        finally:
            mock.close()

    def test_async_runtime_open_and_close_are_single_flight(self) -> None:
        config = self.config("https://api.example.invalid/v1")

        async def concurrent_open() -> None:
            runtime = tny.AsyncRuntime(config, library=self.library)
            first, second = await asyncio.gather(runtime.open(), runtime.open())
            self.assertIs(first, runtime)
            self.assertIs(second, runtime)
            self.assertIs(first._runtime, second._runtime)
            await runtime.close()
            await runtime.close()

        async def open_close_race() -> None:
            runtime = tny.AsyncRuntime(config, library=self.library)
            opened, closed = await asyncio.gather(
                runtime.open(), runtime.close(), return_exceptions=True
            )
            self.assertFalse(isinstance(closed, BaseException), closed)
            if not isinstance(opened, BaseException):
                self.assertIs(opened, runtime)
            self.assertIsNone(runtime._runtime)
            self.assertTrue(runtime._closed)
            with self.assertRaises(tny.BadStateError):
                await runtime.open()

        asyncio.run(concurrent_open())
        asyncio.run(open_close_race())

    def test_cancelled_async_open_reclaims_owner_runtime(self) -> None:
        import tny.aio as aio_module

        config = self.config("https://api.example.invalid/v1")
        original_runtime = aio_module.Runtime
        created: list[tny.Runtime] = []

        class SlowRuntime(original_runtime):
            def __init__(self, *args: object, **kwargs: object) -> None:
                time.sleep(0.1)
                super().__init__(*args, **kwargs)
                created.append(self)

        async def cancel_open_twice() -> None:
            runtime = tny.AsyncRuntime(config, library=self.library)
            task = asyncio.create_task(runtime.open())
            await asyncio.sleep(0.01)
            task.cancel()
            await asyncio.sleep(0.01)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertTrue(runtime._closed)
            self.assertTrue(runtime._executor_shutdown)
            self.assertEqual(runtime._config.api_key, b"")
            await runtime.close()

        aio_module.Runtime = SlowRuntime
        try:
            asyncio.run(cancel_open_twice())
        finally:
            aio_module.Runtime = original_runtime
        self.assertEqual(len(created), 1)
        self.assertTrue(created[0].closed)

    def test_async_runtime_gc_closes_on_owner_executor(self) -> None:
        config = self.config("https://api.example.invalid/v1")

        async def drop_open_runtime() -> tuple[
            weakref.ReferenceType[tny.AsyncRuntime],
            weakref.ReferenceType[tny.Runtime],
        ]:
            runtime = tny.AsyncRuntime(config, library=self.library)
            await runtime.open()
            assert runtime._runtime is not None
            return weakref.ref(runtime), weakref.ref(runtime._runtime)

        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always", ResourceWarning)
            reference, native_reference = asyncio.run(drop_open_runtime())
            for _ in range(50):
                gc.collect()
                if reference() is None and native_reference() is None:
                    break
                time.sleep(0.01)
            self.assertIsNone(reference())
            self.assertIsNone(native_reference())
            released = False
            for _ in range(100):
                try:
                    with tny.Runtime(config, library=self.library):
                        released = True
                    break
                except tny.BusyError:
                    time.sleep(0.01)
            self.assertTrue(released)
            self.assertTrue(any("AsyncRuntime" in str(w.message) for w in caught))

    def test_auth_error_event_and_secret_safe_conformance_report(self) -> None:
        """Conformance: auth_error and report schema/forbidden-field safety."""
        mock = Mock(MOCK_HTTP_STATUS="401", MOCK_ERROR_SECRET="provider-secret")
        try:
            with (
                tny.Runtime(self.config(mock.url), library=self.library) as runtime,
                runtime.create_session() as session,
            ):
                events = list(session.run("trigger auth"))
                errors = [
                    event for event in events if isinstance(event, tny.ErrorEvent)
                ]
                self.assertEqual([event.error_code for event in errors], [-6])
                self.assertNotIn("provider-secret", repr(errors[0]))
                terminals = [
                    event for event in events if isinstance(event, tny.TurnEndEvent)
                ]
                self.assertEqual(len(terminals), 1)
                self.assertEqual(terminals[0].stop_reason, 4)
        finally:
            mock.close()

        scenario_results = {
            "success_two_turns": {"status": "pass", "reason": "assertions_verified"},
            "permission_allow_and_stale_reject": {
                "status": "pass",
                "reason": "assertions_verified",
            },
            "permission_deny": {"status": "pass", "reason": "assertions_verified"},
            "cancel_and_drain": {"status": "pass", "reason": "assertions_verified"},
            "auth_error": {"status": "pass", "reason": "assertions_verified"},
            "unknown_future_event": {"status": "pass", "reason": "assertions_verified"},
            "ownership_and_misuse": {"status": "pass", "reason": "assertions_verified"},
            "slow_consumer_backpressure": {
                "status": "not_run",
                "reason": "fixture_unavailable",
            },
        }
        report = tny.build_conformance_report(
            LIBRARY, library=self.library, scenarios=scenario_results
        )
        encoded = json.dumps(report, sort_keys=True)
        self.assertEqual(
            set(report),
            {
                "abi_version",
                "library_version",
                "sdk",
                "sdk_version",
                "platform",
                "artifact_sha256",
                "capabilities",
                "scenarios",
            },
        )
        for forbidden in (
            "api_key",
            "authorization",
            "cookie",
            "provider_body",
            "provider_headers",
            "provider-secret",
        ):
            self.assertNotIn(forbidden, encoded.lower())
        invalid = dict(scenario_results)
        invalid["slow_consumer_backpressure"] = {
            "status": "not_run",
            "reason": "provider-secret",
        }
        with self.assertRaises(ValueError):
            tny.build_conformance_report(
                LIBRARY, library=self.library, scenarios=invalid
            )

    def test_unknown_future_event_representation(self) -> None:
        """Conformance: unknown_future_event."""
        event = tny.decode_unknown_event_fixture(
            kind=999,
            schema_version=1,
            sequence=7,
            timestamp_ms=8,
            provider=b"future",
            session_id=b"s",
            turn_id=b"t",
            payload={"future_field": b"preserved"},
        )
        self.assertEqual(event.kind, 999)
        self.assertEqual(event.provider, b"future")
        self.assertEqual(event.payload, {"future_field": b"preserved"})
        with self.assertRaises(ValueError):
            tny.decode_unknown_event_fixture(
                kind=0,
                schema_version=1,
                sequence=1,
                timestamp_ms=1,
                provider=b"p",
                session_id=b"s",
                turn_id=b"t",
                payload={},
            )

    def test_canonical_event_schema_matches_sdk_registry(self) -> None:
        registry = json.loads(ROOT.joinpath("sdk/schema/events.json").read_text())
        aliases = {
            "tool_ok": "ok",
            "permission_summary": "summary",
            "permission_options": "options",
            "has_cost": "cost",
        }
        self.assertEqual(
            {event["id"] for event in registry["events"]},
            set(tny.EVENT_TYPES_BY_KIND),
        )
        for definition in registry["events"]:
            event_type, event_class = tny.EVENT_TYPES_BY_KIND[definition["id"]]
            self.assertEqual(event_type, definition["type"])
            attributes = {field.name for field in fields(event_class)}
            for field_name in definition["fields"]:
                self.assertIn(aliases.get(field_name, field_name), attributes)


if __name__ == "__main__":
    unittest.main()
