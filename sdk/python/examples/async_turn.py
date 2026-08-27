"""Run one turn without blocking the asyncio event loop."""
import asyncio
import os

from tny import AsyncRuntime, RuntimeConfig, TextDeltaEvent


async def main() -> None:
    config = RuntimeConfig(
        workspace=os.getcwd(), state_dir=os.path.join(os.getcwd(), ".tny-sdk-state"),
        base_url=os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1"),
        api_key=os.environ["OPENAI_API_KEY"],
    )
    async with AsyncRuntime(config) as runtime:
        async with await runtime.create_session() as session:
            async for event in session.run("Give me a one-line repository summary"):
                if isinstance(event, TextDeltaEvent):
                    print(event.text.decode("utf-8", "strict"), end="")


asyncio.run(main())
