from __future__ import annotations

import asyncio
import unittest
from collections.abc import Iterable

import tny


class FakeRunner:
    def __init__(
        self,
        *,
        delays: dict[str, float] | None = None,
        failures: Iterable[str] = (),
    ) -> None:
        self.delays = delays or {}
        self.failures = set(failures)
        self.active = 0
        self.maximum = 0
        self.prompts: dict[str, bytes] = {}
        self.started: list[str] = []
        self.finished: list[str] = []
        self._lock = asyncio.Lock()

    async def __call__(
        self, task: tny.WorkflowTask, prompt: bytes
    ) -> tny.WorkflowTaskExecution:
        async with self._lock:
            self.active += 1
            self.maximum = max(self.maximum, self.active)
            self.started.append(task.name)
            self.prompts[task.name] = prompt
        try:
            await asyncio.sleep(self.delays.get(task.name, 0))
            if task.name in self.failures:
                raise RuntimeError("fixture task failed")
            return tny.WorkflowTaskExecution(
                output=f"result:{task.name}".encode(),
                session_id=f"session:{task.name}".encode(),
                stop_reason=int(tny.StopReason.DONE),
            )
        finally:
            async with self._lock:
                self.active -= 1
                self.finished.append(task.name)


class WorkflowTests(unittest.IsolatedAsyncioTestCase):
    async def test_parallel_dependency_chain_and_context(self) -> None:
        runner = FakeRunner(delays={"research": 0.05, "tests": 0.05})
        workflow = tny.Workflow(max_concurrency=2, runner=runner)
        returned = workflow.task("research", "audit the API")
        self.assertIs(returned, workflow)
        workflow.add("tests", "audit the tests")
        workflow.task(
            "implement",
            "implement the change",
            depends_on=("research", "tests"),
        )
        workflow.task(
            "ordered",
            "run after implementation",
            depends_on=("implement",),
            include_dependencies=False,
        )

        result = await workflow.run_async()

        self.assertTrue(result.ok)
        self.assertEqual(runner.maximum, 2)
        self.assertEqual(result.output("research"), b"result:research")
        self.assertEqual(result["implement"].session_id, b"session:implement")
        self.assertIn(b'<dependency name="research">', runner.prompts["implement"])
        self.assertIn(b"result:research", runner.prompts["implement"])
        self.assertIn(b"result:tests", runner.prompts["implement"])
        self.assertNotIn(b"tny_workflow_dependencies", runner.prompts["ordered"])
        self.assertLess(
            runner.finished.index("research"), runner.started.index("implement")
        )
        self.assertLess(
            runner.finished.index("tests"), runner.started.index("implement")
        )
        result.raise_for_failure()

    async def test_failure_blocks_only_descendants(self) -> None:
        runner = FakeRunner(failures=("bad",))
        workflow = tny.Workflow(max_concurrency=3, runner=runner)
        workflow.task("bad", "fail")
        workflow.task("independent", "succeed")
        workflow.task("child", "blocked", depends_on=("bad",))
        workflow.task("grandchild", "blocked", depends_on=("child",))

        result = await workflow.run_async()

        self.assertFalse(result.ok)
        self.assertEqual(result["bad"].status, tny.WorkflowTaskStatus.FAILED)
        self.assertEqual(result["independent"].status, tny.WorkflowTaskStatus.SUCCESS)
        self.assertEqual(result["child"].status, tny.WorkflowTaskStatus.BLOCKED)
        self.assertEqual(result["child"].blocked_by, ("bad",))
        self.assertEqual(result["grandchild"].status, tny.WorkflowTaskStatus.BLOCKED)
        self.assertNotIn("child", runner.started)
        with self.assertRaisesRegex(
            tny.WorkflowRunError,
            "bad=failed, child=blocked, grandchild=blocked",
        ):
            result.raise_for_failure()

    async def test_graph_validation_happens_before_execution(self) -> None:
        missing_runner = FakeRunner()
        missing = tny.Workflow(runner=missing_runner)
        missing.task("orphan", "prompt", depends_on=("absent",))
        with self.assertRaisesRegex(tny.WorkflowDefinitionError, "undefined task"):
            await missing.run_async()
        self.assertEqual(missing_runner.started, [])

        cycle_runner = FakeRunner()
        cycle = tny.Workflow(runner=cycle_runner)
        cycle.task("first", "prompt", depends_on=("second",))
        cycle.task("second", "prompt", depends_on=("first",))
        with self.assertRaisesRegex(tny.WorkflowDefinitionError, "cycle detected"):
            await cycle.run_async()
        self.assertEqual(cycle_runner.started, [])

    async def test_context_bound_fails_consumer_without_invoking_it(self) -> None:
        runner = FakeRunner()
        workflow = tny.Workflow(max_dependency_bytes=5, runner=runner)
        workflow.task("producer", "produce")
        workflow.task("consumer", "consume", depends_on=("producer",))

        result = await workflow.run_async()

        self.assertEqual(result["producer"].status, tny.WorkflowTaskStatus.SUCCESS)
        self.assertEqual(result["consumer"].status, tny.WorkflowTaskStatus.FAILED)
        self.assertIsInstance(result["consumer"].error, tny.WorkflowContextError)
        self.assertNotIn("consumer", runner.started)

    async def test_definition_and_repr_are_secret_safe(self) -> None:
        config = tny.RuntimeConfig(
            workspace=".", api_key="API-KEY-SECRET", base_url="https://secret.invalid"
        )
        runner = FakeRunner()
        workflow = tny.Workflow(config, runner=runner)
        workflow.task("safe", "PROMPT-SECRET", runtime_config=config)
        task = workflow.tasks[0]
        execution = tny.WorkflowTaskExecution(
            output=b"OUTPUT-SECRET", error=RuntimeError("ERROR-SECRET")
        )

        rendered = " ".join((repr(workflow), repr(task), repr(execution)))
        for secret in (
            "API-KEY-SECRET",
            "secret.invalid",
            "PROMPT-SECRET",
            "OUTPUT-SECRET",
            "ERROR-SECRET",
        ):
            self.assertNotIn(secret, rendered)

        with self.assertRaises(tny.WorkflowDefinitionError):
            workflow.task("safe", "duplicate")
        with self.assertRaises(tny.WorkflowDefinitionError):
            workflow.task("../escape", "prompt")
        with self.assertRaises(tny.WorkflowDefinitionError):
            workflow.task(123, "prompt")  # type: ignore[arg-type]
        with self.assertRaises(tny.WorkflowDefinitionError):
            workflow.task("invalid-bool", "prompt", include_dependencies=1)  # type: ignore[arg-type]
        with self.assertRaises(tny.WorkflowDefinitionError):
            tny.Workflow(max_concurrency=0, runner=runner)
        duplicate = tny.WorkflowTaskResult(
            name="duplicate", status=tny.WorkflowTaskStatus.SUCCESS
        )
        with self.assertRaises(ValueError):
            tny.WorkflowResult((duplicate, duplicate))

    async def test_cancellation_cleans_active_runners_and_workflow_is_reusable(
        self,
    ) -> None:
        started = asyncio.Event()
        cancelled: list[str] = []

        class CancelRunner:
            async def __call__(
                self, task: tny.WorkflowTask, prompt: bytes
            ) -> tny.WorkflowTaskExecution:
                _ = prompt
                started.set()
                try:
                    await asyncio.sleep(60)
                except asyncio.CancelledError:
                    cancelled.append(task.name)
                    raise

        workflow = tny.Workflow(max_concurrency=2, runner=CancelRunner())
        workflow.task("one", "one").task("two", "two")
        run = asyncio.create_task(workflow.run_async())
        await asyncio.wait_for(started.wait(), timeout=1)
        run.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await run
        self.assertEqual(set(cancelled), {"one", "two"})

        runner = FakeRunner()
        reusable = tny.Workflow(max_concurrency=1, runner=runner).task("again", "again")
        first = await reusable.run_async()
        second = await reusable.run_async()
        self.assertTrue(first.ok)
        self.assertTrue(second.ok)

    async def test_task_execution_validates_at_construction(self) -> None:
        with self.assertRaises(TypeError):
            tny.WorkflowTaskExecution(output="not-bytes")  # type: ignore[arg-type]
        with self.assertRaises(TypeError):
            tny.WorkflowTaskExecution(output=b"ok", session_id="bad")  # type: ignore[arg-type]
        with self.assertRaises(TypeError):
            tny.WorkflowTaskExecution(output=b"ok", stop_reason=999)
        with self.assertRaises(TypeError):
            tny.WorkflowTaskExecution(output=b"ok", error="bad")  # type: ignore[arg-type]

    async def test_non_done_stop_reason_is_failure(self) -> None:
        class DeniedRunner:
            async def __call__(
                self, task: tny.WorkflowTask, prompt: bytes
            ) -> tny.WorkflowTaskExecution:
                _ = task, prompt
                return tny.WorkflowTaskExecution(
                    output=b"partial", stop_reason=int(tny.StopReason.DENIED)
                )

        result = (
            await tny.Workflow(runner=DeniedRunner())
            .task("denied", "prompt")
            .run_async()
        )
        self.assertEqual(result["denied"].status, tny.WorkflowTaskStatus.FAILED)
        self.assertEqual(result["denied"].output, b"partial")
        self.assertIsInstance(result["denied"].error, tny.WorkflowRunError)


class WorkflowSyncTests(unittest.TestCase):
    def test_sync_wrapper_and_mapping_surface(self) -> None:
        result = tny.Workflow(runner=FakeRunner()).task("one", "prompt").run()
        self.assertEqual(tuple(result), ("one",))
        self.assertEqual(len(result), 1)
        self.assertTrue(result["one"].ok)
        self.assertEqual(repr(result), "WorkflowResult(tasks=1, ok=True)")

    def test_sync_wrapper_rejects_active_event_loop(self) -> None:
        async def run() -> None:
            workflow = tny.Workflow(runner=FakeRunner()).task("one", "prompt")
            with self.assertRaisesRegex(tny.WorkflowRunError, "active event loop"):
                workflow.run()

        asyncio.run(run())


if __name__ == "__main__":
    unittest.main()
