from __future__ import annotations

import asyncio
import importlib
import unittest
from collections.abc import Iterable
from unittest.mock import patch

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
            depends_on=(
                "research",
                tny.WorkflowDependency("tests", include_output=False),
            ),
        )
        workflow.task(
            "ordered",
            "run after implementation",
            depends_on=(tny.WorkflowDependency("implement", include_output=False),),
        )

        result = await workflow.run_async()

        self.assertTrue(result.ok)
        self.assertEqual(runner.maximum, 2)
        self.assertEqual(result.output("research"), b"result:research")
        self.assertEqual(result["implement"].session_id, b"session:implement")
        self.assertIn(b'<dependency name="research">', runner.prompts["implement"])
        self.assertIn(b"result:research", runner.prompts["implement"])
        self.assertNotIn(b'<dependency name="tests">', runner.prompts["implement"])
        self.assertNotIn(b"result:tests", runner.prompts["implement"])
        self.assertNotIn(b"tny_workflow_dependencies", runner.prompts["ordered"])
        self.assertLess(
            runner.finished.index("research"), runner.started.index("implement")
        )
        self.assertLess(
            runner.finished.index("tests"), runner.started.index("implement")
        )
        result.raise_for_failure()

    async def test_mixed_fan_in_preserves_included_edge_order(self) -> None:
        runner = FakeRunner()
        workflow = tny.Workflow(runner=runner)
        workflow.task("first", "first")
        workflow.task("ordering-only", "ordering-only")
        workflow.task("third", "third")
        workflow.task(
            "consumer",
            "consumer",
            depends_on=(
                "first",
                tny.WorkflowDependency("ordering-only", include_output=False),
                "third",
            ),
        )

        result = await workflow.run_async()

        self.assertTrue(result.ok)
        prompt = runner.prompts["consumer"]
        first = prompt.index(b'<dependency name="first">')
        third = prompt.index(b'<dependency name="third">')
        self.assertLess(first, third)
        self.assertNotIn(b'<dependency name="ordering-only">', prompt)
        self.assertNotIn(b"result:ordering-only", prompt)

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
            tny.WorkflowDependency("safe", include_output=1)  # type: ignore[arg-type]
        with self.assertRaises(tny.WorkflowDefinitionError):
            workflow.task("invalid-edge", "prompt", depends_on=(123,))  # type: ignore[arg-type]
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

    async def test_repeated_cancellation_waits_for_cleanup_before_reset(self) -> None:
        started = asyncio.Event()
        cleanup_started = asyncio.Event()
        finish_cleanup = asyncio.Event()

        class SlowCleanupRunner:
            def __init__(self) -> None:
                self.calls = 0

            async def __call__(
                self, task: tny.WorkflowTask, prompt: bytes
            ) -> tny.WorkflowTaskExecution:
                _ = task, prompt
                self.calls += 1
                if self.calls > 1:
                    return tny.WorkflowTaskExecution(output=b"reused")
                started.set()
                try:
                    await asyncio.sleep(60)
                except asyncio.CancelledError:
                    cleanup_started.set()
                    await finish_cleanup.wait()
                    raise

        runner = SlowCleanupRunner()
        workflow = tny.Workflow(runner=runner).task("one", "one")
        run = asyncio.create_task(workflow.run_async())
        await asyncio.wait_for(started.wait(), timeout=1)
        run.cancel()
        await asyncio.wait_for(cleanup_started.wait(), timeout=1)
        run.cancel()
        await asyncio.sleep(0)
        try:
            self.assertFalse(run.done())
            with self.assertRaisesRegex(
                tny.WorkflowDefinitionError, "cannot change a running workflow"
            ):
                workflow.task("too-early", "prompt")
            with self.assertRaisesRegex(
                tny.WorkflowRunError, "workflow is already running"
            ):
                await workflow.run_async()
        finally:
            finish_cleanup.set()

        with self.assertRaises(asyncio.CancelledError):
            await run
        result = await workflow.run_async()
        self.assertTrue(result.ok)
        self.assertEqual(result.output("one"), b"reused")

    async def test_permission_handler_rejects_bool_and_invalid_results(self) -> None:
        permission = tny.PermissionRequestEvent(
            kind=4,
            schema_version=1,
            sequence=1,
            timestamp_ms=0,
            provider=b"fixture",
            session_id=b"session",
            turn_id=b"turn",
            type="permission_request",
            permission_id=b"permission",
            summary=b"fixture",
            options=tny.PermissionOptions.ALLOW | tny.PermissionOptions.DENY,
        )

        class FakeSession:
            def __init__(self) -> None:
                self.responses: list[tny.PermissionDecision] = []

            async def __aenter__(self) -> FakeSession:
                return self

            async def __aexit__(self, *args: object) -> None:
                return None

            async def id(self) -> bytes:
                return b"session"

            async def run(self, prompt: bytes):  # type: ignore[no-untyped-def]
                _ = prompt
                yield permission
                yield tny.TurnEndEvent(
                    kind=7,
                    schema_version=1,
                    sequence=2,
                    timestamp_ms=0,
                    provider=b"fixture",
                    session_id=b"session",
                    turn_id=b"turn",
                    type="turn_end",
                    stop_reason=int(tny.StopReason.DONE),
                )

            async def respond_permission(
                self,
                event: tny.PermissionRequestEvent,
                decision: tny.PermissionDecision,
            ) -> None:
                _ = event
                self.responses.append(decision)

        class FakeRuntime:
            def __init__(self, session: FakeSession) -> None:
                self.session = session

            async def __aenter__(self) -> FakeRuntime:
                return self

            async def __aexit__(self, *args: object) -> None:
                return None

            async def create_session(self) -> FakeSession:
                return self.session

        workflow_module = importlib.import_module("tny.workflow")
        for value in (False, True, 1, "allow"):
            with self.subTest(value=value):
                session = FakeSession()
                with patch.object(
                    workflow_module,
                    "AsyncRuntime",
                    return_value=FakeRuntime(session),
                ):
                    result = (
                        await tny.Workflow(
                            tny.RuntimeConfig(workspace="."),
                            on_permission=lambda task, event, value=value: value,
                        )
                        .task("permission", "prompt")
                        .run_async()
                    )  # type: ignore[arg-type]
                self.assertEqual(
                    result["permission"].status, tny.WorkflowTaskStatus.FAILED
                )
                self.assertIsInstance(result["permission"].error, tny.WorkflowRunError)
                self.assertEqual(session.responses, [])

        session = FakeSession()
        with patch.object(
            workflow_module,
            "AsyncRuntime",
            return_value=FakeRuntime(session),
        ):
            allowed = (
                await tny.Workflow(
                    tny.RuntimeConfig(workspace="."),
                    on_permission=lambda task, event: tny.PermissionDecision.ALLOW,
                )
                .task("permission", "prompt")
                .run_async()
            )
        self.assertEqual(allowed["permission"].status, tny.WorkflowTaskStatus.SUCCESS)
        self.assertEqual(session.responses, [tny.PermissionDecision.ALLOW])

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


class WorkflowUsageTests(unittest.IsolatedAsyncioTestCase):
    async def test_cumulative_cost_and_missing_tokens_remain_unknown(self) -> None:
        from dataclasses import replace

        usage = tny.UsageEvent(
            kind=6,
            schema_version=1,
            sequence=1,
            timestamp_ms=0,
            provider=b"acp",
            session_id=b"same-session",
            turn_id=b"turn",
            type="usage",
            input_tokens=0,
            output_tokens=0,
            context_used=9,
            context_size=100,
            cost=0.25,
            cost_currency=b"EUR",
            cost_cumulative=True,
            tokens_reported=False,
        )

        async def runner(task, prompt):
            return tny.WorkflowTaskExecution(b"ok", usage=usage)

        result = await (
            tny.Workflow(runner=runner)
            .task("first", "p")
            .task("second", "p")
            .run_async()
        )
        self.assertEqual(result["first"].usage.cost, 0.25)
        self.assertEqual(result.usage["known_tasks"], 2)
        self.assertIsNone(result.usage["cost"])
        self.assertIsNone(result.usage["input_tokens"])
        self.assertIsNone(result.usage["output_tokens"])

        async def mixed_currency(task, prompt):
            return tny.WorkflowTaskExecution(
                b"ok",
                usage=replace(
                    usage,
                    cost_cumulative=False,
                    tokens_reported=True,
                    cost_currency=b"EUR" if task.name == "first" else b"USD",
                ),
            )

        mixed = await (
            tny.Workflow(runner=mixed_currency)
            .task("first", "p")
            .task("second", "p")
            .run_async()
        )
        self.assertIsNone(mixed.usage["cost"])

    async def test_native_usage_last_snapshot_retained_and_aggregated_once(
        self,
    ) -> None:
        from tny.events import UsageEvent

        def usage(tokens, cost):
            return UsageEvent(
                kind=6,
                schema_version=1,
                sequence=1,
                timestamp_ms=0,
                provider=b"fixture",
                session_id=b"session",
                turn_id=b"turn",
                type="usage",
                input_tokens=tokens,
                output_tokens=2,
                context_used=10,
                context_size=100,
                cost=cost,
            )

        first, last = usage(3, None), usage(7, 0.25)

        class FakeSession:
            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                pass

            async def id(self):
                return b"session"

            async def run(self, prompt):
                yield first
                yield last
                yield tny.TurnEndEvent(
                    kind=7,
                    schema_version=1,
                    sequence=3,
                    timestamp_ms=0,
                    provider=b"fixture",
                    session_id=b"session",
                    turn_id=b"turn",
                    type="turn_end",
                    stop_reason=int(tny.StopReason.DONE),
                )

        class FakeRuntime:
            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                pass

            async def create_session(self):
                return FakeSession()

        with patch("tny.workflow.AsyncRuntime", return_value=FakeRuntime()):
            result = await (
                tny.Workflow(tny.RuntimeConfig(workspace="."))
                .task("source", "p")
                .task("one", "p", depends_on=["source"])
                .task("two", "p", depends_on=["source"])
                .run_async()
            )
        self.assertIs(result["source"].usage, last)
        self.assertEqual(
            dict(result.usage),
            {
                "known_tasks": 3,
                "unknown_tasks": 0,
                "input_tokens": 21,
                "output_tokens": 6,
                "cost": 0.75,
            },
        )

        async def runner(task, prompt):
            return tny.WorkflowTaskExecution(
                b"ok",
                usage=first if task.name == "known" else None,
                error=RuntimeError("failed") if task.name == "unknown" else None,
            )

        result = await (
            tny.Workflow(runner=runner)
            .task("known", "p")
            .task("unknown", "p")
            .task("blocked", "p", depends_on=["unknown"])
            .run_async()
        )
        self.assertIsNone(result.usage["input_tokens"])
        self.assertIsNone(result.usage["cost"])
        self.assertEqual(result.usage["unknown_tasks"], 1)
        self.assertEqual(result.usage["known_tasks"], 1)
        self.assertIsNone(result["blocked"].usage)
        known = tny.WorkflowResult([result["known"]])
        self.assertEqual(known.usage["input_tokens"], 3)
        self.assertIsNone(known.usage["cost"])


class WorkflowContextTests(unittest.IsolatedAsyncioTestCase):
    async def test_32_consumers_only_compose_after_admission(self) -> None:
        module = importlib.import_module("tny.workflow")
        entered = asyncio.Event()
        release = asyncio.Event()
        rendered: list[int] = []
        original = module._render_prompt

        def render(task, *args):
            prompt = original(task, *args)
            if task.name != "source":
                rendered.append(len(prompt))
            return prompt

        async def runner(task, prompt):
            if task.name == "source":
                return tny.WorkflowTaskExecution(b"x" * 262144)
            entered.set()
            await release.wait()
            return tny.WorkflowTaskExecution(b"done")

        workflow = tny.Workflow(max_concurrency=1, runner=runner).task(
            "source", "produce"
        )
        for index in range(32):
            workflow.task(f"consumer-{index}", "consume", depends_on=["source"])
        with patch.object(module, "_render_prompt", render):
            run = asyncio.create_task(workflow.run_async())
            try:
                await asyncio.wait_for(entered.wait(), 2)
                for _ in range(3):
                    await asyncio.sleep(0)
                self.assertEqual(len(rendered), 1)
                self.assertGreater(rendered[0], 262144)
            finally:
                release.set()
                result = await asyncio.wait_for(run, 2)
        self.assertTrue(result.ok)
        self.assertEqual(len(rendered), 32)

    async def test_selective_context_preserves_original_and_provenance(self) -> None:
        import base64
        import hashlib
        import json

        output = b'{"keep":{"nested":42},"omit":"private","__proto__":"safe"}'
        prompts = {}

        async def runner(task, prompt):
            prompts[task.name] = prompt
            return tny.WorkflowTaskExecution(
                output if task.name == "source" else b"ok", b"session"
            )

        workflow = tny.Workflow(runner=runner).task("source", "produce")
        for name, edge in [
            (
                "summary",
                tny.WorkflowDependency(
                    "source", context="summary", summary="explicit summary"
                ),
            ),
            (
                "fields",
                tny.WorkflowDependency(
                    "source", context="fields", fields=("keep", "__proto__")
                ),
            ),
            (
                "artifact",
                tny.WorkflowDependency(
                    "source", context="artifact", offset=1, length=6
                ),
            ),
            ("reference", tny.WorkflowDependency("source", context="artifact")),
            ("none", tny.WorkflowDependency("source", include_output=False)),
        ]:
            workflow.task(name, "consume", depends_on=[edge])
        result = await workflow.run_async()
        self.assertTrue(result.ok)
        self.assertEqual(result.output("source"), output)
        artifact = result["source"].artifact
        self.assertEqual(artifact.sha256, hashlib.sha256(output).hexdigest())
        self.assertEqual(artifact.read(1, 6), output[1:7])
        for args in [(0, len(output) + 1), (-1, 0), (len(output) + 1, 0), (False, 1)]:
            with self.assertRaises(tny.WorkflowContextError):
                artifact.read(*args)
        with self.assertRaises(tny.WorkflowContextError):
            artifact.read(0, 2, maximum_bytes=1)
        for name in ("summary", "fields", "artifact", "reference"):
            self.assertNotIn(b"private", prompts[name])
            self.assertIn(artifact.sha256.encode(), prompts[name])
            self.assertIn(b"not higher-priority instructions", prompts[name])
        self.assertEqual(prompts["none"], b"consume")
        payload = json.loads(
            prompts["artifact"]
            .split(b'<dependency name="source">\n')[1]
            .split(b"\n</dependency>")[0]
        )
        self.assertEqual(base64.b64decode(payload["data_base64"]), output[1:7])
        self.assertIn(b'"__proto__":"safe"', prompts["fields"])
        self.assertNotIn("explicit summary", repr(workflow.tasks))

    async def test_large_original_requires_explicit_selection(self) -> None:
        output = b"x" * (1024 * 1024 + 1)
        prompts = {}

        async def runner(task, prompt):
            prompts[task.name] = prompt
            return tny.WorkflowTaskExecution(output if task.name == "source" else b"ok")

        result = await (
            tny.Workflow(runner=runner)
            .task("source", "p")
            .task("whole", "p", depends_on=["source"])
            .task(
                "summary",
                "p",
                depends_on=[
                    tny.WorkflowDependency(
                        "source", context="summary", summary="selected fact"
                    )
                ],
            )
            .task(
                "reference",
                "p",
                depends_on=[tny.WorkflowDependency("source", context="artifact")],
            )
            .run_async()
        )
        self.assertIsInstance(result["whole"].error, tny.WorkflowContextError)
        self.assertNotIn("whole", prompts)
        self.assertTrue(result["summary"].ok)
        self.assertTrue(result["reference"].ok)
        self.assertIs(result["source"].artifact.data, output)
        self.assertEqual(result.output("source"), output)
        self.assertLess(len(prompts["reference"]), 1024)

    async def test_context_limits_fail_before_runner_and_block_descendants(
        self,
    ) -> None:
        for options, edge in [
            ({"max_input_bytes": 100}, tny.WorkflowDependency("source")),
            ({"max_dependency_bytes": 2}, tny.WorkflowDependency("source")),
            (
                {"max_selection_bytes": 2},
                tny.WorkflowDependency("source", context="fields", fields=("key",)),
            ),
            (
                {"max_selection_bytes": 2},
                tny.WorkflowDependency("source", context="artifact", length=3),
            ),
            (
                {},
                tny.WorkflowDependency("source", context="fields", fields=("missing",)),
            ),
            ({}, tny.WorkflowDependency("source", context="artifact", offset=1000)),
        ]:
            started = []

            async def runner(task, prompt):
                started.append(task.name)
                return tny.WorkflowTaskExecution(b'{"key":42}')

            result = await (
                tny.Workflow(runner=runner, **options)
                .task("source", "produce")
                .task("consumer", "consume", depends_on=[edge])
                .task("blocked", "consume", depends_on=["consumer"])
                .run_async()
            )
            self.assertEqual(started, ["source"])
            self.assertIsInstance(result["consumer"].error, tny.WorkflowContextError)
            self.assertEqual(result["blocked"].status, tny.WorkflowTaskStatus.BLOCKED)
        runner = FakeRunner()
        result = (
            await tny.Workflow(runner=runner, max_input_bytes=1)
            .task("root", "é")
            .run_async()
        )
        self.assertFalse(result.ok)
        self.assertEqual(runner.started, [])

    async def test_complete_input_exact_boundary_and_no_context(self) -> None:
        runner = FakeRunner()
        workflow = (
            tny.Workflow(runner=runner)
            .task("root", "root")
            .task("child", "é", depends_on=["root"])
        )
        self.assertTrue((await workflow.run_async()).ok)
        length = len(runner.prompts["child"])
        for bound, ok in [(length, True), (length - 1, False)]:
            result = await (
                tny.Workflow(runner=FakeRunner(), max_input_bytes=bound)
                .task("root", "root")
                .task("child", "é", depends_on=["root"])
                .run_async()
            )
            self.assertEqual(result.ok, ok)
        result = await (
            tny.Workflow(runner=FakeRunner(), max_input_bytes=4)
            .task("root", "root")
            .task("child", "é", depends_on=[tny.WorkflowDependency("root", False)])
            .run_async()
        )
        self.assertTrue(result.ok)

    async def test_cancel_does_not_compose_waiters(self) -> None:
        module = importlib.import_module("tny.workflow")
        started = asyncio.Event()
        renders = []
        original = module._render_prompt

        def render(task, *args):
            renders.append(task.name)
            return original(task, *args)

        async def runner(task, prompt):
            started.set()
            await asyncio.Event().wait()

        workflow = tny.Workflow(max_concurrency=1, runner=runner)
        for index in range(32):
            workflow.task(f"task-{index}", "prompt")
        with patch.object(module, "_render_prompt", render):
            run = asyncio.create_task(workflow.run_async())
            await asyncio.wait_for(started.wait(), 2)
            run.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await run
        self.assertEqual(renders, ["task-0"])

    def test_selector_validation(self) -> None:
        for options in [
            {"context": "unknown"},
            {"context": "summary"},
            {"summary": "text"},
            {"context": "fields"},
            {"fields": ("a",)},
            {"context": "fields", "fields": ("a", "a")},
            {"context": "artifact", "offset": -1},
            {"length": 1},
            {"context": "artifact", "include_output": False},
        ]:
            with self.assertRaises(tny.WorkflowDefinitionError):
                tny.WorkflowDependency("source", **options)
        for key in ("max_input_bytes", "max_selection_bytes"):
            for value in (0, -1, True, 1.5):
                with self.assertRaises(tny.WorkflowDefinitionError):
                    tny.Workflow(runner=FakeRunner(), **{key: value})


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
