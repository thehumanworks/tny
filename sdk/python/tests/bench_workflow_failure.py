"""Measure retained allocations and runner-frame liveness after 32 failures.

Run with PYTHONPATH pointing at baseline or candidate sdk/python/src.
This is traced Python allocation evidence, not process RSS or provider latency.
"""

import asyncio
import gc
import json
import tracemalloc
import weakref

import tny


class Sentinel:
    pass


async def main():
    refs = []

    async def runner(task, prompt):
        if task.name == "source":
            return tny.WorkflowTaskExecution(b"x" * 262144)
        local = Sentinel()
        refs.append(weakref.ref(local))
        raise ValueError("runner diagnostic")

    workflow = tny.Workflow(runner=runner, max_concurrency=1).task("source", "p")
    for index in range(32):
        workflow.task(f"c{index}", "p", depends_on=["source"])
    tracemalloc.start()
    result = await workflow.run_async()
    gc.collect()
    retained, peak = tracemalloc.get_traced_memory()
    prompts = {}
    for failed in result.failed:
        traceback = failed.error.__traceback__
        while traceback:
            prompt = traceback.tb_frame.f_locals.get("prompt")
            if isinstance(prompt, bytes):
                prompts[id(prompt)] = len(prompt)
            traceback = traceback.tb_next
    print(
        json.dumps(
            {
                "failed_tasks": len(result.failed),
                "retained_bytes": retained,
                "peak_bytes": peak,
                "live_runner_locals": sum(ref() is not None for ref in refs),
                "distinct_frame_prompt_bytes": sum(prompts.values()),
            }
        )
    )
    tracemalloc.stop()


if __name__ == "__main__":
    asyncio.run(main())
