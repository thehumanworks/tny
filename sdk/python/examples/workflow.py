"""Fan out two native agents, then feed both results to a final task."""

from __future__ import annotations

import asyncio
import os

import tny


async def main() -> None:
    config = tny.RuntimeConfig(
        workspace=os.getcwd(),
        base_url=os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1"),
        api_key=os.environ.get("OPENAI_API_KEY"),
        permission_mode=tny.PermissionMode.ASK,
    )
    workflow = tny.Workflow(config, max_concurrency=2)
    workflow.task("architecture", "Audit this repository's architecture.")
    workflow.task("tests", "Audit this repository's test coverage.")
    workflow.task(
        "plan",
        "Create one implementation plan grounded in both reports.",
        depends_on=("architecture", "tests"),
    )
    result = await workflow.run_async()
    result.raise_for_failure()
    print(result.output("plan").decode("utf-8", "strict"))


if __name__ == "__main__":
    asyncio.run(main())
