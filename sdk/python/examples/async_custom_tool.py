"""Use asyncio custom-tool completion with synchronous host services."""
import asyncio
import os
import time

import tny


async def invoke(arguments_json: bytes) -> bytes:
    await asyncio.sleep(0)
    return b"application result: " + arguments_json


async def main() -> None:
    services = tny.HostServices(
        monotonic_ms=lambda: time.monotonic_ns() // 1_000_000,
        notify_scheduler=lambda: None,
    )
    async with tny.AsyncRuntime(tny.RuntimeConfig(
        workspace=".", base_url="https://api.openai.com/v1",
        api_key=os.environ["OPENAI_API_KEY"],
        permission_mode=tny.PermissionMode.YOLO,
    ), host_services=services) as runtime:
        registration = await runtime.register_tool(tny.AsyncCustomTool(
            name=b"host_async",
            description=b"Resolve an application-owned asynchronous operation.",
            input_schema_json=b'{"type":"object","properties":{}}',
            handler=invoke,
        ))
        session = await runtime.create_session()
        async for event in session.run(b"Use host_async"):
            print(event.type)
        await session.close()
        await registration.close()


asyncio.run(main())
