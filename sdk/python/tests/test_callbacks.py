from __future__ import annotations

import asyncio
import gc
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
import weakref
from pathlib import Path
from typing import Any, cast

import tny
import tny.callbacks as callback_module

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
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


class Mock:
    def __init__(self) -> None:
        self.port = free_port()
        self.process = subprocess.Popen(
            [sys.executable, os.fspath(MOCK), str(self.port)],
            env=dict(
                os.environ, MOCK_EXPECT_WIRE="responses", MOCK_CUSTOM_TOOL="host_echo"
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert self.process.stdout is not None
        if b"ready" not in self.process.stdout.readline():
            raise RuntimeError("mock did not become ready")

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}/v1"

    def close(self) -> None:
        self.process.terminate()
        self.process.wait(timeout=5)
        if self.process.stdout:
            self.process.stdout.close()
        if self.process.stderr:
            self.process.stderr.close()


class CallbackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.workspace = Path(self.temporary.name, "workspace")
        self.workspace.mkdir()
        self.library = tny.Library(LIBRARY)
        self.assertEqual(self.library.abi_major, 1)
        self.assertGreaterEqual(self.library.abi_minor, 1)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def config(self, url: str = "http://127.0.0.1:1/v1") -> tny.RuntimeConfig:
        return tny.RuntimeConfig(
            workspace=self.workspace,
            base_url=url,
            api_key="callback-test-secret",
            permission_mode=tny.PermissionMode.YOLO,
        )

    def test_callback_layouts_and_abi1_services(self) -> None:
        ffi = self.library.ffi
        self.assertEqual(ffi.sizeof("tny_host_services_v1"), 136)
        self.assertEqual(ffi.offsetof("tny_host_services_v1", "reserved"), 72)
        self.assertEqual(ffi.sizeof("tny_runtime_options_v1"), 280)
        self.assertEqual(ffi.offsetof("tny_runtime_options_v1", "host_services"), 208)
        self.assertEqual(ffi.sizeof("tny_tool_result_v1"), 64)
        self.assertEqual(ffi.offsetof("tny_tool_result_v1", "data"), 8)
        self.assertEqual(ffi.sizeof("tny_tool_spec_v1"), 160)
        self.assertEqual(ffi.offsetof("tny_tool_spec_v1", "invoke"), 88)
        self.assertEqual(ffi.sizeof("tny_capabilities_v1"), 344)
        self.assertEqual(ffi.offsetof("tny_capabilities_v1", "reserved"), 280)
        runtime = tny.Runtime(
            self.config(),
            library=self.library,
            host_services=tny.HostServices(monotonic_ms=lambda: 7),
        )
        try:
            self.assertEqual(runtime.host_monotonic_ms(), 7)
        finally:
            runtime.close()

    def test_host_services_stress_failures_reentrancy_and_isolation(self) -> None:
        calls: list[tuple[str, bytes]] = []
        nested: list[type[BaseException]] = []
        holder: dict[str, tny.Runtime] = {}
        counter = 1000
        stored = (0, b"")

        def clock() -> int:
            nonlocal counter
            counter += 1
            gc.collect()
            if "runtime" in holder:
                try:
                    holder["runtime"].host_notify_scheduler()
                except BaseException as error:
                    nested.append(type(error))
            return counter

        def random_bytes(size: int) -> bytes:
            return b"R" * size

        def load(key: bytes) -> tuple[int, bytes]:
            calls.append(("load", key))
            return stored

        def store(key: bytes, revision: int, data: bytes) -> int:
            nonlocal stored
            calls.append(("store", key))
            self.assertEqual(revision, stored[0])
            stored = (revision + 1, data)
            return stored[0]

        services = tny.HostServices(
            diagnostic=lambda level, component, message: calls.append(
                (f"diagnostic-{level}", component + b":" + message)
            ),
            monotonic_ms=clock,
            secure_random=random_bytes,
            storage_load=load,
            storage_store=store,
            open_url=lambda url: calls.append(("url", url)),
            notify_scheduler=lambda: calls.append(("notify", b"")),
        )
        self.assertNotIn("callback-test-secret", repr(services))
        runtime = tny.Runtime(
            self.config(), library=self.library, host_services=services
        )
        holder["runtime"] = runtime
        for _ in range(2000):
            self.assertGreater(runtime.host_monotonic_ms(), 0)
        self.assertTrue(nested and all(item is tny.BadStateError for item in nested))
        self.assertEqual(runtime.host_secure_random(32), b"R" * 32)
        revision = runtime.host_storage_store(b"opaque-key", 0, b"\xffbytes")
        self.assertEqual(
            runtime.host_storage_load(b"opaque-key"), (revision, b"\xffbytes")
        )
        runtime.host_open_url(b"scheme:opaque")
        first_count = len(calls)
        runtime.close()
        closed_count = len(calls)
        self.assertGreater(closed_count, first_count)
        gc.collect()
        self.assertEqual(len(calls), closed_count)

        first = tny.Runtime(
            self.config(),
            library=self.library,
            host_services=tny.HostServices(monotonic_ms=lambda: 11),
        )
        second = tny.Runtime(
            self.config(),
            library=self.library,
            host_services=tny.HostServices(monotonic_ms=lambda: 22),
        )
        try:
            self.assertEqual(first.host_monotonic_ms(), 11)
            self.assertEqual(second.host_monotonic_ms(), 22)
        finally:
            second.close()
            first.close()

    def test_host_handler_exception_is_redacted(self) -> None:
        def fail(_size: int) -> bytes:
            raise RuntimeError("CALLBACK-SECRET-MUST-NOT-ESCAPE")

        with tny.Runtime(
            self.config(),
            library=self.library,
            host_services=tny.HostServices(secure_random=fail),
        ) as runtime:
            with self.assertRaises(tny.InternalError) as caught:
                runtime.host_secure_random(8)
        rendered = f"{caught.exception!s} {caught.exception!r}"
        self.assertNotIn("CALLBACK-SECRET", rendered)

    def test_sync_custom_tool_lifetime_reentrancy_and_unregister(self) -> None:
        mock = Mock()
        invocations: list[bytes] = []
        reentrant: list[type[BaseException]] = []
        runtime = tny.Runtime(self.config(mock.url), library=self.library)

        class Handler:
            def __call__(self, arguments: bytes) -> tny.ToolResult:
                invocations.append(arguments)
                gc.collect()
                try:
                    runtime.host_monotonic_ms()
                except BaseException as error:
                    reentrant.append(type(error))
                return tny.ToolResult(b"\xffhost-result")

        handler = Handler()
        reference = weakref.ref(handler)
        tool = tny.CustomTool(
            name=b"host_echo",
            description=b"opaque SECRET-DESCRIPTION",
            input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
            handler=handler,
            max_argument_bytes=1024,
            max_result_bytes=1024,
        )
        registration = runtime.register_tool(tool)
        self.assertNotIn("SECRET-DESCRIPTION", repr(tool))
        self.assertNotIn("host-result", repr(registration))
        del handler, tool
        gc.collect()
        self.assertIsNotNone(reference())
        try:
            with runtime.create_session() as session:
                events = list(session.run(b"call host tool"))
                self.assertTrue(
                    any(isinstance(event, tny.TurnEndEvent) for event in events)
                )
            self.assertEqual(invocations, [b'{"value":"hello"}'])
            self.assertEqual(reentrant, [tny.BadStateError])
            self.assertTrue(runtime.capabilities.custom_tool_callbacks)
            self.assertEqual(runtime.capabilities.custom_tool_max_count, 64)
            registration.close()
            self.assertTrue(registration.closed)
        finally:
            runtime.close()
            mock.close()
        gc.collect()
        self.assertIsNone(reference())

    def test_invalid_sync_results_fail_explicitly_and_repr_is_redacted(self) -> None:
        invalid_handlers: list[tuple[str, Any, int]] = [
            ("nul", lambda _value: b"bad\0result", 1024),
            ("utf8", lambda _value: b"\xff", 1024),
            ("oversize", lambda _value: b"too-long", 4),
            ("type", lambda _value: cast(Any, "not-bytes"), 1024),
        ]
        for label, handler, maximum in invalid_handlers:
            with self.subTest(label=label):
                mock = Mock()
                runtime = tny.Runtime(self.config(mock.url), library=self.library)
                result = tny.ToolResult(b"TOP-SECRET-RESULT")
                self.assertNotIn("TOP-SECRET", repr(result))
                runtime.register_tool(
                    tny.CustomTool(
                        name=b"host_echo",
                        description=b"invalid result",
                        input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                        handler=handler,
                        max_argument_bytes=1024,
                        max_result_bytes=maximum,
                    )
                )
                try:
                    with runtime.create_session() as session:
                        events = list(session.run(b"invoke invalid result"))
                    self.assertTrue(
                        any(
                            isinstance(event, tny.ToolEndEvent) and not event.ok
                            for event in events
                        )
                    )
                finally:
                    runtime.close()
                    mock.close()

    def test_registration_rolls_back_and_closed_registration_refreshes_capabilities(
        self,
    ) -> None:
        runtime = tny.Runtime(self.config(), library=self.library)
        original = self.library.read_capabilities

        class Handler:
            def __call__(self, value: bytes) -> bytes:
                return value

        handler = Handler()
        reference = weakref.ref(handler)
        tool = tny.CustomTool(
            name=b"host_echo",
            description=b"rollback",
            input_schema_json=b'{"type":"object"}',
            handler=handler,
        )

        def fail_refresh(*_args: object, **_kwargs: object) -> tny.Capabilities:
            raise MemoryError("injected Python bookkeeping failure")

        self.library.read_capabilities = fail_refresh  # type: ignore[assignment]
        try:
            with self.assertRaises(MemoryError):
                runtime.register_tool(tool)
        finally:
            self.library.read_capabilities = original  # type: ignore[assignment]
        self.assertEqual(runtime._registrations, [])
        registration = runtime.register_tool(tool)
        self.assertTrue(runtime.capabilities.custom_tool_callbacks)
        registration.close()
        self.assertFalse(runtime.capabilities.custom_tool_callbacks)
        del handler, tool, registration
        gc.collect()
        self.assertIsNone(reference())
        runtime.close()

    def test_custom_tool_exception_never_unwinds_or_exposes_text(self) -> None:
        mock = Mock()
        runtime = tny.Runtime(self.config(mock.url), library=self.library)

        def fail(_arguments: bytes) -> bytes:
            raise KeyboardInterrupt("CUSTOM-HANDLER-SECRET")

        runtime.register_tool(
            tny.CustomTool(
                name=b"host_echo",
                description=b"fail safely",
                input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                handler=fail,
                max_argument_bytes=1024,
                max_result_bytes=1024,
            )
        )
        try:
            with runtime.create_session() as session:
                events = list(session.run(b"invoke failing host tool"))
            rendered = repr(events)
            self.assertNotIn("CUSTOM-HANDLER-SECRET", rendered)
            self.assertTrue(
                any(
                    isinstance(event, tny.ToolEndEvent) and not event.ok
                    for event in events
                )
            )
            self.assertEqual(
                sum(isinstance(event, tny.TurnEndEvent) for event in events), 1
            )
        finally:
            runtime.close()
            mock.close()


class AsyncCallbackTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.workspace = Path(self.temporary.name, "workspace")
        self.workspace.mkdir()
        self.library = tny.Library(LIBRARY)
        self.assertEqual(self.library.abi_major, 1)
        self.assertGreaterEqual(self.library.abi_minor, 1)

    async def asyncTearDown(self) -> None:
        self.temporary.cleanup()

    async def test_async_custom_tool_completion_and_cancel_close_race(self) -> None:
        mock = Mock()
        started = asyncio.Event()
        release = asyncio.Event()
        seen: list[bytes] = []

        async def handler(arguments: bytes) -> bytes:
            seen.append(arguments)
            started.set()
            await release.wait()
            return b"async-result"

        runtime = tny.AsyncRuntime(
            tny.RuntimeConfig(
                workspace=self.workspace,
                base_url=mock.url,
                api_key="async-callback-secret",
                permission_mode=tny.PermissionMode.YOLO,
            ),
            library=self.library,
            host_services=tny.HostServices(notify_scheduler=lambda: None),
        )
        await runtime.open()
        self.assertIsNone(runtime._host_services)
        registration = await runtime.register_tool(
            tny.AsyncCustomTool(
                name=b"host_echo",
                description=b"async",
                input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                handler=handler,
                max_argument_bytes=1024,
                max_result_bytes=1024,
            )
        )
        session = await runtime.create_session()

        async def consume() -> list[tny.AnyEvent]:
            return [event async for event in session.run(b"call async host tool")]

        task = asyncio.create_task(consume())
        await asyncio.wait_for(started.wait(), timeout=10)
        await session.cancel()
        release.set()
        events = await asyncio.wait_for(task, timeout=10)
        self.assertEqual(seen, [b'{"value":"hello"}'])
        self.assertEqual(
            sum(isinstance(event, tny.TurnEndEvent) for event in events), 1
        )
        await session.close()
        await registration.close()
        await runtime.close()
        self.assertIsNone(runtime._host_services)
        mock.close()

    async def test_runtime_close_waits_for_handler_that_suppresses_cancel(self) -> None:
        mock = Mock()
        started = asyncio.Event()
        suppressed = asyncio.Event()
        release = asyncio.Event()

        async def handler(_arguments: bytes) -> bytes:
            started.set()
            try:
                await asyncio.Event().wait()
            except asyncio.CancelledError:
                suppressed.set()
                await release.wait()
            return b"finished-after-cancel"

        runtime = tny.AsyncRuntime(
            tny.RuntimeConfig(
                workspace=self.workspace,
                base_url=mock.url,
                api_key="close-race-secret",
                permission_mode=tny.PermissionMode.YOLO,
            ),
            library=self.library,
        )
        registration = await runtime.register_tool(
            tny.AsyncCustomTool(
                name=b"host_echo",
                description=b"pending",
                input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                handler=handler,
                max_argument_bytes=1024,
                max_result_bytes=1024,
            )
        )
        session = await runtime.create_session()
        consumer = asyncio.create_task(
            anext(session.run(b"invoke and close pending host tool"))
        )
        await asyncio.wait_for(started.wait(), timeout=10)
        closing = asyncio.create_task(runtime.close())
        await asyncio.wait_for(suppressed.wait(), timeout=10)
        self.assertFalse(closing.done())
        release.set()
        await asyncio.wait_for(closing, timeout=10)
        self.assertTrue(registration.closed)
        if not consumer.done():
            consumer.cancel()
        try:
            await consumer
        except BaseException:
            pass
        mock.close()

    async def test_completion_oom_retries_redacted_fallback(self) -> None:
        mock = Mock()

        async def handler(_arguments: bytes) -> bytes:
            return b"ordinary-result"

        runtime = tny.AsyncRuntime(
            tny.RuntimeConfig(
                workspace=self.workspace,
                base_url=mock.url,
                api_key="oom-secret",
                permission_mode=tny.PermissionMode.YOLO,
            ),
            library=self.library,
        )
        registration = await runtime.register_tool(
            tny.AsyncCustomTool(
                name=b"host_echo",
                description=b"oom",
                input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                handler=handler,
                max_argument_bytes=1024,
                max_result_bytes=1024,
            )
        )
        sync = registration._sync
        original = sync._complete_native
        completions = 0

        def inject_once(call: Any, generation: int, result: tny.ToolResult) -> int:
            nonlocal completions
            completions += 1
            if completions == 1:
                return -4
            return original(call, generation, result)

        sync._complete_native = inject_once
        session = await runtime.create_session()
        events = [event async for event in session.run(b"invoke completion oom")]
        self.assertEqual(completions, 2)
        self.assertTrue(
            any(
                isinstance(event, tny.ToolEndEvent) and not event.ok for event in events
            )
        )
        await runtime.close()
        mock.close()

    async def test_repeated_completion_oom_requests_safe_cancellation(self) -> None:
        mock = Mock()

        async def handler(_arguments: bytes) -> bytes:
            return b"ordinary-result"

        runtime = tny.AsyncRuntime(
            tny.RuntimeConfig(
                workspace=self.workspace,
                base_url=mock.url,
                api_key="oom-cancel-secret",
                permission_mode=tny.PermissionMode.YOLO,
            ),
            library=self.library,
        )
        registration = await runtime.register_tool(
            tny.AsyncCustomTool(
                name=b"host_echo",
                description=b"oom twice",
                input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                handler=handler,
                max_argument_bytes=1024,
                max_result_bytes=1024,
            )
        )
        sync = registration._sync
        cancel = sync._request_cancel
        cancellations = 0

        def record_cancel() -> bool:
            nonlocal cancellations
            cancellations += 1
            return cancel()

        sync._complete_native = lambda _call, _generation, _result: -4
        sync._request_cancel = record_cancel
        session = await runtime.create_session()
        events = [
            event async for event in session.run(b"invoke repeated completion oom")
        ]
        self.assertEqual(cancellations, 1)
        terminals = [event for event in events if isinstance(event, tny.TurnEndEvent)]
        self.assertEqual(len(terminals), 1)
        self.assertEqual(terminals[0].stop_reason, 1)
        await runtime.close()
        mock.close()

    async def test_failed_completion_and_cancel_retires_after_close_invalidation(
        self,
    ) -> None:
        mock = Mock()
        attempted = threading.Event()

        async def handler(_arguments: bytes) -> bytes:
            return b"ordinary-result"

        runtime = tny.AsyncRuntime(
            tny.RuntimeConfig(
                workspace=self.workspace,
                base_url=mock.url,
                api_key="false-cancel-secret",
                permission_mode=tny.PermissionMode.YOLO,
            ),
            library=self.library,
        )
        registration = await runtime.register_tool(
            tny.AsyncCustomTool(
                name=b"host_echo",
                description=b"false cancel",
                input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                handler=handler,
                max_argument_bytes=1024,
                max_result_bytes=1024,
            )
        )
        sync = registration._sync

        def fail_complete(_call: Any, _generation: int, _result: tny.ToolResult) -> int:
            attempted.set()
            return -4

        sync._complete_native = fail_complete
        sync._request_cancel = lambda: False
        session = await runtime.create_session()
        consumer = asyncio.create_task(
            anext(session.run(b"invoke and force close invalidation"))
        )
        self.assertTrue(await asyncio.to_thread(attempted.wait, 10))
        await asyncio.wait_for(runtime.close(), timeout=10)
        self.assertTrue(registration.closed)
        self.assertEqual(sync._pending, {})
        if not consumer.done():
            consumer.cancel()
        try:
            await consumer
        except BaseException:
            pass
        mock.close()

    async def test_scheduled_handler_publication_rolls_back_every_stage(self) -> None:
        stages = ("before_insert", "after_insert", "after_done_callback", "before_arm")
        for stage in stages:
            with self.subTest(stage=stage):
                mock = Mock()

                async def handler(_arguments: bytes) -> bytes:
                    await asyncio.sleep(0)
                    return b"unused"

                runtime = tny.AsyncRuntime(
                    tny.RuntimeConfig(
                        workspace=self.workspace,
                        base_url=mock.url,
                        api_key="publication-secret",
                        permission_mode=tny.PermissionMode.YOLO,
                    ),
                    library=self.library,
                )
                registration = await runtime.register_tool(
                    tny.AsyncCustomTool(
                        name=b"host_echo",
                        description=b"publication rollback",
                        input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                        handler=handler,
                        max_argument_bytes=1024,
                        max_result_bytes=1024,
                    )
                )
                sync = registration._sync

                def inject(observed: str, *, target: str = stage) -> None:
                    if observed == target:
                        raise MemoryError(f"injected {target}")

                sync._publication_hook = inject
                session = await runtime.create_session()
                events = [
                    event async for event in session.run(b"invoke publication rollback")
                ]
                self.assertEqual(sync._pending, {})
                self.assertTrue(
                    any(
                        isinstance(event, tny.ToolEndEvent) and not event.ok
                        for event in events
                    )
                )
                await runtime.close()
                mock.close()
        await asyncio.sleep(0)
        self.assertFalse(
            any(
                thread.is_alive() and thread.name == "libtny-python-tool-completion"
                for thread in threading.enumerate()
            )
        )

    async def test_never_started_coroutine_is_closed_without_native_authority_calls(
        self,
    ) -> None:
        mock = Mock()
        handler_started = False
        captured: list[Any] = []

        async def body() -> bytes:
            nonlocal handler_started
            handler_started = True
            return b"unreachable"

        def handler(_arguments: bytes) -> Any:
            coroutine = body()
            captured.append(coroutine)
            return coroutine

        class ControlledFuture:
            def __init__(self, runner: Any) -> None:
                self.runner = runner
                self.was_cancelled = False
                self.callbacks = 0

            def cancel(self) -> bool:
                self.was_cancelled = True
                self.runner.close()
                return True

            def cancelled(self) -> bool:
                return self.was_cancelled

            def add_done_callback(self, _callback: Any) -> None:
                self.callbacks += 1

        controlled: list[ControlledFuture] = []
        original_schedule = callback_module.asyncio.run_coroutine_threadsafe

        def schedule(runner: Any, _loop: Any) -> ControlledFuture:
            future = ControlledFuture(runner)
            controlled.append(future)
            return future

        runtime = tny.AsyncRuntime(
            tny.RuntimeConfig(
                workspace=self.workspace,
                base_url=mock.url,
                api_key="never-started-secret",
                permission_mode=tny.PermissionMode.YOLO,
            ),
            library=self.library,
        )
        registration = await runtime.register_tool(
            tny.AsyncCustomTool(
                name=b"host_echo",
                description=b"never start",
                input_schema_json=b'{"type":"object","properties":{"value":{"type":"string"}}}',
                handler=handler,
                max_argument_bytes=1024,
                max_result_bytes=1024,
            )
        )
        sync = registration._sync
        completes = 0
        releases = 0

        def no_complete(_call: Any, _generation: int, _result: tny.ToolResult) -> int:
            nonlocal completes
            completes += 1
            return 0

        def no_release(_call: Any) -> None:
            nonlocal releases
            releases += 1

        sync._complete_native = no_complete
        sync._release_native = no_release
        sync._publication_hook = lambda stage: (
            (_ for _ in ()).throw(MemoryError("injected before insert"))
            if stage == "before_insert"
            else None
        )
        callback_module.asyncio.run_coroutine_threadsafe = schedule
        try:
            session = await runtime.create_session()
            events = [
                event async for event in session.run(b"invoke never-started rollback")
            ]
        finally:
            callback_module.asyncio.run_coroutine_threadsafe = original_schedule
        self.assertEqual(len(controlled), 1)
        self.assertTrue(controlled[0].was_cancelled)
        self.assertEqual(controlled[0].callbacks, 0)
        self.assertFalse(handler_started)
        self.assertEqual(len(captured), 1)
        self.assertIsNone(captured[0].cr_frame)
        self.assertEqual(sync._pending, {})
        self.assertEqual((completes, releases), (0, 0))
        self.assertTrue(
            any(
                isinstance(event, tny.ToolEndEvent) and not event.ok for event in events
            )
        )
        await runtime.close()
        mock.close()


if __name__ == "__main__":
    unittest.main()
