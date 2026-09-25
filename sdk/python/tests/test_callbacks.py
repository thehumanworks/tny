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


def assert_execution_refused(
    test: unittest.TestCase, events: list[tny.AnyEvent]
) -> None:
    failures = [event for event in events if isinstance(event, tny.ToolEndEvent)]
    test.assertEqual(len(failures), 1)
    test.assertEqual(failures[0].tool_name, b"run_code")
    test.assertFalse(failures[0].ok)
    test.assertIn(b"execution server unavailable", failures[0].tool_detail)
    test.assertIn(b"no direct fallback", failures[0].tool_detail)
    terminals = [event for event in events if isinstance(event, tny.TurnEndEvent)]
    test.assertEqual(len(terminals), 1)
    test.assertEqual(terminals[0].stop_reason, 0)


def invoke_callback(registration: Any) -> int:
    """Exercise the Python CFFI boundary, without a model or native call authority."""
    ffi = registration._runtime.library.ffi
    arguments = b'{"value":"hello"}'
    buffer = ffi.new("char[]", arguments)
    view = ffi.new("tny_bytes *", {"ptr": buffer, "len": len(arguments)})
    result = ffi.new("tny_tool_result_v1 *")
    return int(
        registration._callback_ref(
            registration._handle_ref, ffi.NULL, 7, view[0], result
        )
    )


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

    def test_model_refusal_preserves_custom_tool_lifetime_and_unregister(self) -> None:
        mock = Mock()
        invocations: list[bytes] = []
        runtime = tny.Runtime(self.config(mock.url), library=self.library)

        self_workspace = self.workspace

        class Handler:
            def __call__(self, arguments: bytes) -> tny.ToolResult:
                invocations.append(arguments)
                (self_workspace / "callback-effect").write_bytes(arguments)
                return tny.ToolResult(b"host-result")

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
                assert_execution_refused(self, events)
            self.assertEqual(invocations, [])
            self.assertFalse((self.workspace / "callback-effect").exists())
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
                runtime = tny.Runtime(self.config(), library=self.library)
                result = tny.ToolResult(b"TOP-SECRET-RESULT")
                self.assertNotIn("TOP-SECRET", repr(result))
                registration = runtime.register_tool(
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
                    self.assertEqual(
                        invoke_callback(registration),
                        callback_module.STATUS_INVALID_ARGUMENT,
                    )
                finally:
                    runtime.close()

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

    def test_sync_callback_exception_and_reentrancy_at_callback_boundary(self) -> None:
        with tny.Runtime(self.config(), library=self.library) as runtime:
            nested: list[type[BaseException]] = []

            def fail(_arguments: bytes) -> bytes:
                try:
                    runtime.host_monotonic_ms()
                except BaseException as error:
                    nested.append(type(error))
                raise KeyboardInterrupt("CUSTOM-HANDLER-SECRET")

            registration = runtime.register_tool(
                tny.CustomTool(
                    name=b"host_echo",
                    description=b"fail safely",
                    input_schema_json=b'{"type":"object"}',
                    handler=fail,
                )
            )
            self.assertEqual(
                invoke_callback(registration), callback_module.STATUS_INTERNAL
            )
            self.assertEqual(nested, [tny.BadStateError])
            self.assertNotIn("CUSTOM-HANDLER-SECRET", repr(registration))
            self.assertEqual(runtime._callback_depth, 0)


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

    async def boundary(
        self, handler: Any
    ) -> tuple[Any, Any, list[tny.ToolResult], threading.Event]:
        """Register normally, but isolate callback mechanics from native execution.

        The NULL call in invoke_callback has no native completion/release authority.
        Every boundary test replaces both operations before invoking the callback.
        """
        runtime = tny.AsyncRuntime(
            tny.RuntimeConfig(workspace=self.workspace), library=self.library
        )
        self.addAsyncCleanup(runtime.close)
        registration = await runtime.register_tool(
            tny.AsyncCustomTool(
                name=b"host_echo",
                description=b"isolated callback boundary",
                input_schema_json=b'{"type":"object"}',
                handler=handler,
            )
        )
        completions: list[tny.ToolResult] = []
        released = threading.Event()

        def complete(_call: Any, generation: int, result: tny.ToolResult) -> int:
            self.assertEqual(generation, 7)
            completions.append(result)
            return 0

        registration._sync._complete_native = complete
        registration._sync._release_native = lambda _call: released.set()
        registration._sync._request_cancel = lambda: False
        return runtime, registration, completions, released

    async def test_model_refuses_async_tool_without_callback_or_file_effects(
        self,
    ) -> None:
        mock = Mock()
        self.addCleanup(mock.close)
        seen: list[bytes] = []

        async def handler(arguments: bytes) -> bytes:
            seen.append(arguments)
            (self.workspace / "async-effect").write_bytes(arguments)
            return b"unexpected"

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
        self.addAsyncCleanup(runtime.close)
        await runtime.open()
        self.assertIsNone(runtime._host_services)
        registration = await runtime.register_tool(
            tny.AsyncCustomTool(
                name=b"host_echo",
                description=b"async",
                input_schema_json=b'{"type":"object"}',
                handler=handler,
            )
        )
        session = await runtime.create_session()
        events = [event async for event in session.run(b"call async host tool")]
        assert_execution_refused(self, events)
        self.assertEqual(seen, [])
        self.assertFalse((self.workspace / "async-effect").exists())
        self.assertEqual(registration._sync._pending, {})
        await session.close()
        await registration.close()
        self.assertTrue(registration.closed)
        await runtime.close()
        self.assertIsNone(runtime._host_services)

    async def test_async_completion_at_callback_boundary(self) -> None:
        seen: list[bytes] = []

        async def handler(arguments: bytes) -> bytes:
            seen.append(arguments)
            return b"async-result"

        runtime, registration, completions, released = await self.boundary(handler)
        self.assertEqual(await runtime._call(invoke_callback, registration._sync), 1)
        self.assertTrue(await asyncio.to_thread(released.wait, 5))
        await registration.close()
        self.assertEqual(seen, [b'{"value":"hello"}'])
        self.assertEqual(completions, [tny.ToolResult(b"async-result")])
        self.assertEqual(registration._sync._pending, {})

    async def test_runtime_close_waits_for_handler_that_suppresses_cancel(self) -> None:
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

        runtime, registration, completions, released = await self.boundary(handler)
        self.assertEqual(await runtime._call(invoke_callback, registration._sync), 1)
        await asyncio.wait_for(started.wait(), timeout=5)
        closing = asyncio.create_task(runtime.close())
        try:
            await asyncio.wait_for(suppressed.wait(), timeout=5)
            self.assertFalse(closing.done())
        finally:
            release.set()
            await asyncio.wait_for(closing, timeout=10)
        self.assertTrue(registration.closed)
        self.assertTrue(released.is_set())
        self.assertEqual(completions, [tny.ToolResult(b"finished-after-cancel")])

    async def test_completion_oom_retries_redacted_fallback(self) -> None:
        async def handler(_arguments: bytes) -> bytes:
            return b"ordinary-result"

        runtime, registration, _, released = await self.boundary(handler)
        completions: list[tny.ToolResult] = []

        def inject_once(_call: Any, _generation: int, result: tny.ToolResult) -> int:
            completions.append(result)
            return -4 if len(completions) == 1 else 0

        registration._sync._complete_native = inject_once
        self.assertEqual(await runtime._call(invoke_callback, registration._sync), 1)
        self.assertTrue(await asyncio.to_thread(released.wait, 5))
        await registration.close()
        self.assertEqual(
            completions,
            [
                tny.ToolResult(b"ordinary-result"),
                tny.ToolResult(b"custom tool completion failed", is_error=True),
            ],
        )

    async def test_repeated_completion_oom_requests_safe_cancellation(self) -> None:
        async def handler(_arguments: bytes) -> bytes:
            return b"ordinary-result"

        runtime, registration, _, released = await self.boundary(handler)
        cancellations: list[bool] = []
        completions: list[tny.ToolResult] = []

        def fail_complete(_call: Any, _generation: int, result: tny.ToolResult) -> int:
            completions.append(result)
            return -4

        def cancel() -> bool:
            cancellations.append(True)
            return True

        registration._sync._complete_native = fail_complete
        registration._sync._request_cancel = cancel
        self.assertEqual(await runtime._call(invoke_callback, registration._sync), 1)
        self.assertTrue(await asyncio.to_thread(released.wait, 5))
        await registration.close()
        self.assertEqual(cancellations, [True])
        self.assertEqual(len(completions), 2)
        self.assertEqual(registration._sync._pending, {})

    async def test_failed_completion_and_cancel_retires_after_close_invalidation(
        self,
    ) -> None:
        async def handler(_arguments: bytes) -> bytes:
            return b"ordinary-result"

        runtime, registration, _, released = await self.boundary(handler)
        # An open native session keeps authority alive until runtime close.
        await runtime.create_session()
        attempted = threading.Event()

        def cancel() -> bool:
            attempted.set()
            return False

        registration._sync._complete_native = lambda _call, _generation, _result: -4
        registration._sync._request_cancel = cancel
        self.assertEqual(await runtime._call(invoke_callback, registration._sync), 1)
        self.assertTrue(await asyncio.to_thread(attempted.wait, 5))
        self.assertFalse(released.is_set())
        await asyncio.wait_for(runtime.close(), timeout=10)
        self.assertTrue(registration.closed)
        self.assertTrue(released.is_set())
        self.assertEqual(registration._sync._pending, {})

    async def test_scheduled_handler_publication_rolls_back_every_stage(self) -> None:
        stages = ("before_insert", "after_insert", "after_done_callback", "before_arm")
        for stage in stages:
            with self.subTest(stage=stage):

                async def handler(_arguments: bytes) -> bytes:
                    await asyncio.sleep(0)
                    return b"unused"

                runtime, registration, completions, released = await self.boundary(
                    handler
                )
                sync = registration._sync

                def inject(observed: str, *, target: str = stage) -> None:
                    if observed == target:
                        raise MemoryError(f"injected {target}")

                sync._publication_hook = inject
                self.assertEqual(
                    await runtime._call(invoke_callback, sync),
                    callback_module.STATUS_INTERNAL,
                )
                self.assertEqual(sync._pending, {})
                self.assertEqual(completions, [])
                self.assertFalse(released.is_set())
                await runtime.close()
        self.assertFalse(
            any(
                thread.is_alive() and thread.name == "libtny-python-tool-completion"
                for thread in threading.enumerate()
            )
        )

    async def test_never_started_coroutine_is_closed_without_native_authority_calls(
        self,
    ) -> None:
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

        runtime, registration, completions, released = await self.boundary(handler)
        sync = registration._sync

        def inject(stage: str) -> None:
            if stage == "before_insert":
                raise MemoryError("injected before insert")

        sync._publication_hook = inject
        callback_module.asyncio.run_coroutine_threadsafe = schedule
        try:
            self.assertEqual(
                await runtime._call(invoke_callback, sync),
                callback_module.STATUS_INTERNAL,
            )
        finally:
            callback_module.asyncio.run_coroutine_threadsafe = original_schedule
        self.assertEqual(len(controlled), 1)
        self.assertTrue(controlled[0].was_cancelled)
        self.assertEqual(controlled[0].callbacks, 0)
        self.assertFalse(handler_started)
        self.assertEqual(len(captured), 1)
        self.assertIsNone(captured[0].cr_frame)
        self.assertEqual(sync._pending, {})
        self.assertEqual(completions, [])
        self.assertFalse(released.is_set())
        await runtime.close()


if __name__ == "__main__":
    unittest.main()
