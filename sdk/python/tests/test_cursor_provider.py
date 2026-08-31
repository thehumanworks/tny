from __future__ import annotations

import asyncio
import json
import os
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

import tny

ROOT = Path(__file__).resolve().parents[3]
MOCK = ROOT / "tests" / "integration" / "mock_bridge.py"
LIBRARY = Path(
    os.environ.get(
        "TNY_TEST_LIBRARY",
        ROOT
        / "build"
        / "lib"
        / ("libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1"),
    )
)
TURN_TIMEOUT = float(
    os.environ.get("TNY_TEST_TURN_TIMEOUT", "60" if sys.platform == "darwin" else "15")
)


@contextmanager
def cursor_environment(
    workspace: Path, state: Path, *, invoke_tool: bool
) -> Iterator[None]:
    wrapper = state / "cursor-sdk-bridge"
    wrapper.write_text(
        f'#!/bin/sh\nexec "{sys.executable}" -u "{MOCK}" "$@"\n',
        encoding="utf-8",
    )
    wrapper.chmod(0o700)
    values = {
        "CURSOR_SDK_BRIDGE_BIN": os.fspath(wrapper),
        "CURSOR_API_KEY": "cursor-sdk-test-key-not-real",
        "TNY_MOCK_DIR": os.fspath(state / "bridge"),
        "TNY_MOCK_CWD": os.fspath(workspace),
        "TNY_MOCK_INVOKE_CUSTOM_TOOL": "host_echo" if invoke_tool else None,
    }
    if sys.platform == "darwin":
        # Hosted macOS Python processes can take over 30 seconds to import the
        # bridge fixture before it emits its ready line.
        values["TNY_CURSOR_BRIDGE_READY_TIMEOUT_MS"] = "60000"
    Path(values["TNY_MOCK_DIR"]).mkdir()
    previous = {name: os.environ.get(name) for name in values}
    try:
        for name, value in values.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        yield
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


class CursorProviderTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="tny-sdk-cursor-")
        self.root = Path(self.temporary.name)
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.library = tny.Library(LIBRARY)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def config(self, state: Path) -> tny.RuntimeConfig:
        return tny.RuntimeConfig(
            workspace=self.workspace,
            state_dir=state,
            provider="cursor",
            model="mock-cursor-model",
            api_key="cursor-sdk-test-key-not-real",
            permission_mode=tny.PermissionMode.YOLO,
        )

    def test_cursor_environment_restores_ready_timeout_override(self) -> None:
        name = "TNY_CURSOR_BRIDGE_READY_TIMEOUT_MS"
        previous = os.environ.get(name)
        os.environ[name] = "ambient-sentinel"
        state = self.root / "environment"
        state.mkdir()
        try:
            with cursor_environment(self.workspace, state, invoke_tool=False):
                expected = "60000" if sys.platform == "darwin" else "ambient-sentinel"
                self.assertEqual(os.environ.get(name), expected)
            self.assertEqual(os.environ.get(name), "ambient-sentinel")
        finally:
            if previous is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = previous

    async def test_sync_and_async_tools_round_trip_through_cursor_callback(
        self,
    ) -> None:
        for asynchronous in (False, True):
            state = self.root / ("async" if asynchronous else "sync")
            state.mkdir()
            seen: list[bytes] = []

            async def async_handler(arguments: bytes) -> bytes:
                seen.append(arguments)
                await asyncio.sleep(0.01)
                return b'{"kind":"async"}'

            def sync_handler(arguments: bytes) -> bytes:
                seen.append(arguments)
                return b'{"kind":"sync"}'

            tool_type = tny.AsyncCustomTool if asynchronous else tny.CustomTool
            handler = async_handler if asynchronous else sync_handler
            tool = tool_type(
                name="host_echo",
                description="Cursor callback fixture",
                input_schema_json=(
                    '{"type":"object","properties":{"text":{"type":"string"},'
                    '"count":{"type":"integer"}}}'
                ),
                handler=handler,
            )
            with cursor_environment(self.workspace, state, invoke_tool=True):
                runtime = tny.AsyncRuntime(self.config(state), library=self.library)
                async with runtime:
                    registration = await runtime.register_tool(tool)
                    async with registration:
                        session = await runtime.create_session()
                        async with session:
                            events = await asyncio.wait_for(
                                self._collect(session.run("invoke the host tool")),
                                timeout=TURN_TIMEOUT,
                            )
                            self.assertIsNotNone(runtime._runtime)
                            assert runtime._runtime is not None
                            self.assertEqual(
                                runtime._runtime.capabilities.provider, "cursor"
                            )
                            current = await runtime._call(
                                runtime._runtime.library.read_capabilities,
                                runtime._runtime._handle,
                                extended=True,
                            )
                            self.assertTrue(current.provider_initialized)
                            self.assertTrue(
                                all(event.provider == b"cursor" for event in events)
                            )
                            self.assertEqual(len(seen), 1)
                            arguments = json.loads(seen[0])
                            self.assertEqual(
                                arguments["text"], "mock custom tool input"
                            )
                            self.assertEqual(arguments["count"], 1)
                            self.assertEqual(
                                sum(
                                    isinstance(event, tny.TurnEndEvent)
                                    for event in events
                                ),
                                1,
                            )
            failures = state / "bridge" / "failures.log"
            self.assertFalse(
                failures.exists(), failures.read_text() if failures.exists() else ""
            )

    async def test_registered_runtime_cancels_and_tears_down_cursor_host(self) -> None:
        state = self.root / "cancel"
        state.mkdir()
        started = asyncio.Event()
        release = asyncio.Event()

        async def pending_handler(_arguments: bytes) -> bytes:
            started.set()
            await release.wait()
            return b'{"cancel":"released"}'

        with cursor_environment(self.workspace, state, invoke_tool=True):
            runtime = tny.AsyncRuntime(self.config(state), library=self.library)
            async with runtime:
                registration = await runtime.register_tool(
                    tny.AsyncCustomTool(
                        name="host_echo",
                        description="registered during cancellation",
                        input_schema_json='{"type":"object"}',
                        handler=pending_handler,
                    )
                )
                async with registration:
                    session = await runtime.create_session()
                    async with session:
                        consumer = asyncio.create_task(
                            self._collect(session.run("cancel this Cursor run"))
                        )
                        try:
                            await self._wait_for_handler_or_run_end(started, consumer)
                            await session.cancel()
                            release.set()
                            events = await asyncio.wait_for(
                                asyncio.shield(consumer), timeout=TURN_TIMEOUT
                            )
                        finally:
                            release.set()
                            if not consumer.done():
                                try:
                                    await session.cancel()
                                except tny.BadStateError:
                                    pass
                                consumer.cancel()
                            await asyncio.gather(consumer, return_exceptions=True)
                        terminals = [
                            event
                            for event in events
                            if isinstance(event, tny.TurnEndEvent)
                        ]
                        self.assertEqual(len(terminals), 1)
                        routes = (state / "bridge" / "routes.log").read_text()
                        self.assertIn("/sdk.v1.SdkAgentService/CancelRun", routes)
                        self.assertEqual(terminals[0].stop_reason, 1)
        failures = state / "bridge" / "failures.log"
        self.assertFalse(
            failures.exists(), failures.read_text() if failures.exists() else ""
        )

    @staticmethod
    async def _collect(events: object) -> list[tny.AnyEvent]:
        return [event async for event in events]  # type: ignore[attr-defined]

    async def _wait_for_handler_or_run_end(
        self,
        started: asyncio.Event,
        consumer: asyncio.Task[list[tny.AnyEvent]],
    ) -> None:
        """Wait for the lifecycle edge without assuming bridge startup latency."""
        started_waiter = asyncio.create_task(started.wait())
        try:
            done, _ = await asyncio.wait(
                (started_waiter, consumer),
                timeout=TURN_TIMEOUT,
                return_when=asyncio.FIRST_COMPLETED,
            )
            if not done:
                self.fail(
                    "Cursor custom-tool handler did not start before turn timeout"
                )
            if consumer in done:
                await consumer
                self.fail("Cursor run ended before invoking the custom-tool handler")
        finally:
            if not started_waiter.done():
                started_waiter.cancel()
            await asyncio.gather(started_waiter, return_exceptions=True)


if __name__ == "__main__":
    unittest.main()
