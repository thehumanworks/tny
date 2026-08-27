"""Public import surface checked with mypy --strict."""
from __future__ import annotations

import tny


def host_clock() -> int:
    return 1


def sync_tool(arguments: bytes) -> tny.ToolResult:
    return tny.ToolResult(arguments, is_error=False)


async def async_tool(arguments: bytes) -> bytes:
    return arguments


def consume(runtime: tny.Runtime, session: tny.Session, event: tny.AnyEvent) -> bytes:
    config: tny.RuntimeConfig = runtime.config
    if isinstance(event, tny.TextDeltaEvent):
        return event.text
    if isinstance(event, tny.UnknownEvent):
        value = event.payload.get("text", b"")
        return value if isinstance(value, bytes) else b""
    token = tny.CancellationToken()
    token.cancel()
    _ = config
    _ = session
    return b""


def callback_types(config: tny.RuntimeConfig) -> None:
    services = tny.HostServices(monotonic_ms=host_clock)
    runtime = tny.Runtime(config, host_services=services)
    registration: tny.ToolRegistration = runtime.register_tool(tny.CustomTool(
        name=b"host_echo", description=b"echo",
        input_schema_json=b'{"type":"object"}', handler=sync_tool,
    ))
    _: int = runtime.host_monotonic_ms()
    registration.close()
    runtime.close()


async def async_callback_types(config: tny.RuntimeConfig) -> None:
    runtime = tny.AsyncRuntime(config, host_services=tny.HostServices())
    registration: tny.AsyncToolRegistration = await runtime.register_tool(
        tny.AsyncCustomTool(
            name=b"host_async", description=b"async",
            input_schema_json=b'{"type":"object"}', handler=async_tool,
        )
    )
    await registration.close()
    await runtime.close()
