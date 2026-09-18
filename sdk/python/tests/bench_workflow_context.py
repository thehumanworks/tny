"""Offline 32x256KiB benchmark; optional baseline workflow.py argument.

Fresh processes: tracemalloc peak covers the complete run. RSS includes SDK
imports. Neither variant makes a provider/network call.
"""

import asyncio
import gc
import importlib.util
import json
import resource
import sys
import time
import tracemalloc

import tny.workflow as candidate


def load(path):
    spec = importlib.util.spec_from_file_location("tny.workflow_baseline", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


async def benchmark(module):
    entered = asyncio.Event()
    release = asyncio.Event()
    renders = 0
    composed_bytes = 0
    original = module._render_prompt

    def render(task, *args):
        nonlocal renders, composed_bytes
        prompt = original(task, *args)
        if task.name != "source":
            renders += 1
            composed_bytes += len(prompt)
        return prompt

    module._render_prompt = render

    async def runner(task, prompt):
        if task.name == "source":
            return module.WorkflowTaskExecution(b"x" * 262144)
        entered.set()
        await release.wait()
        return module.WorkflowTaskExecution(b"ok")

    gc.collect()
    tracemalloc.start()
    started = time.perf_counter()
    workflow = module.Workflow(max_concurrency=1, runner=runner).task(
        "source", "produce"
    )
    for index in range(32):
        workflow.task(f"consumer-{index}", "consume", depends_on=["source"])
    run = asyncio.create_task(workflow.run_async())
    await asyncio.wait_for(entered.wait(), 10)
    for _ in range(3):
        await asyncio.sleep(0)
    barrier = {
        "renders": renders,
        "composed_bytes": composed_bytes,
        "live_python_bytes": tracemalloc.get_traced_memory()[0],
        "latency_ms": (time.perf_counter() - started) * 1000,
    }
    release.set()
    result = await asyncio.wait_for(run, 10)
    assert result.ok
    latency = (time.perf_counter() - started) * 1000
    peak = tracemalloc.get_traced_memory()[1]
    tracemalloc.stop()
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    print(
        json.dumps(
            {
                "variant": "baseline" if len(sys.argv) > 1 else "candidate",
                "barrier": barrier,
                "peak_python_bytes": peak,
                "peak_rss_bytes": rss if sys.platform == "darwin" else rss * 1024,
                "total_composed_bytes": composed_bytes,
                "total_ms": latency,
            }
        )
    )


if __name__ == "__main__":
    asyncio.run(benchmark(load(sys.argv[1]) if len(sys.argv) > 1 else candidate))
