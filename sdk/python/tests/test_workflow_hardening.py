"""Review regressions: accounting, JSON selection and prompt ownership."""

import asyncio
import gc
import tracemalloc
import unittest
import weakref
from unittest.mock import patch

import tny


def usage(tokens=7):
    return tny.UsageEvent(
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
        cost=0.25,
    )


class HardeningTests(unittest.IsolatedAsyncioTestCase):
    async def test_native_failure_and_late_cancellation_accounting(self):
        for mode in ("stream", "cleanup", "cancel"):
            entered = asyncio.Event()
            workflow = tny.Workflow(tny.RuntimeConfig(workspace=".")).task("a", "p")

            class Session:
                async def __aenter__(self):
                    return self

                async def __aexit__(self, *args):
                    if mode == "cancel":
                        self.last_usage = usage(9)
                    if mode == "cleanup":
                        raise RuntimeError("close failed")

                async def id(self):
                    return b"session"

                async def run(self, prompt):
                    yield usage(3)
                    yield usage()
                    entered.set()
                    if mode == "cancel":
                        await asyncio.Event().wait()
                    if mode == "stream":
                        raise ValueError("stream failed")

            class Runtime(Session):
                async def __aexit__(self, *args):
                    pass

                async def create_session(self):
                    return Session()

            with patch("tny.workflow.AsyncRuntime", return_value=Runtime()):
                running = asyncio.create_task(workflow.run_async())
                await entered.wait()
                before = getattr(workflow, "partial_usage", None)
                if mode == "cancel":
                    running.cancel()
                    with self.assertRaises(asyncio.CancelledError):
                        await running
                    self.assertEqual(workflow.partial_usage["input_tokens"], 9)
                else:
                    result = await running
                    self.assertFalse(result.ok)
                    self.assertEqual(result.usage["input_tokens"], 7)
                    self.assertEqual(result.usage["cost"], 0.25)
                self.assertEqual(before["input_tokens"], 7)
                self.assertEqual(workflow.partial_usage["known_tasks"], 1)

    async def test_async_session_accounts_for_discarded_drain_events(self):
        from tny.aio import AsyncSession

        events = iter([usage(3), usage(9)])

        class Sync:
            closed = False

            def cancel(self):
                pass

            def next_event(self, *args, **kwargs):
                return next(events)

        class Runtime:
            async def _call(self, function):
                return function()

        session = AsyncSession(Runtime(), Sync())
        await session._cancel_and_drain()
        self.assertEqual(session.last_usage.input_tokens, 9)

    async def test_whole_source_json_policy(self):
        invalid = [
            b"[]",
            b"null",
            b"{",
            b'{"keep":1,"omit":' + b"[" * 130 + b"0" + b"]" * 130 + b"}",
        ]
        for token in (b"NaN", b"Infinity", b"-Infinity", b"1e400", b"9" * 400):
            invalid += [
                b'{"keep":' + token + b"}",
                b'{"keep":1,"omit":' + token + b"}",
                b'{"keep":1,"omit":' + token + b',"omit":0}',
            ]
        for source in invalid + [
            b'{"keep":1,"keep":2}',
            b'{"keep":9007199254740991}',
            b'{"keep":"1e400"}',
        ]:
            prompts = {}

            async def runner(task, prompt):
                prompts[task.name] = prompt
                return tny.WorkflowTaskExecution(source)

            result = await (
                tny.Workflow(runner=runner)
                .task("a", "p")
                .task(
                    "b",
                    "p",
                    depends_on=[
                        tny.WorkflowDependency("a", context="fields", fields=("keep",))
                    ],
                )
                .run_async()
            )
            with self.subTest(source=source):
                self.assertEqual(result.ok, source not in invalid, source)
                if source in invalid:
                    self.assertIsInstance(result["b"].error, tny.WorkflowContextError)
                elif source == b'{"keep":1,"keep":2}':
                    self.assertIn(b'"fields":{"keep":2}', prompts["b"])

    async def test_failed_consumers_release_frames_and_prompts(self):
        class Sentinel:
            pass

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
        try:
            result = await workflow.run_async()
            gc.collect()
            retained, _ = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertLess(retained, 2 * 1024 * 1024)
        self.assertTrue(all(ref() is None for ref in refs))
        for failed in result.failed:
            self.assertIsInstance(failed.error, ValueError)
            self.assertEqual(str(failed.error), "runner diagnostic")
            self.assertIsNone(failed.error.__traceback__)

    async def test_cancellation_releases_runner_frame(self):
        class Sentinel:
            pass

        refs = []
        entered = asyncio.Event()

        async def runner(task, prompt):
            local = Sentinel()
            refs.append(weakref.ref(local))
            entered.set()
            await asyncio.Event().wait()

        workflow = tny.Workflow(runner=runner).task("a", "x" * 262144)
        running = asyncio.create_task(workflow.run_async())
        await entered.wait()
        running.cancel()
        cancellation = None
        try:
            await running
        except asyncio.CancelledError as error:
            cancellation = error
        self.assertIsInstance(cancellation, asyncio.CancelledError)
        gc.collect()
        self.assertTrue(all(ref() is None for ref in refs))
        self.assertEqual(workflow.partial_usage["unknown_tasks"], 1)

    async def test_each_selection_exact_bounds(self):
        source = b'{"keep":"value","omit":42}'
        for edge in (
            tny.WorkflowDependency("a", context="summary", summary="é"),
            tny.WorkflowDependency("a", context="fields", fields=("keep",)),
            tny.WorkflowDependency("a", context="artifact", offset=1, length=5),
        ):
            prompts = {}

            async def runner(task, prompt):
                prompts[task.name] = prompt
                return tny.WorkflowTaskExecution(source, session_id=b"session")

            def flow(**options):
                return (
                    tny.Workflow(runner=runner, **options)
                    .task("a", "p")
                    .task("b", "é", depends_on=[edge])
                )

            self.assertTrue((await flow().run_async()).ok)
            prompt = prompts["b"]
            selected = prompt.split(b'<dependency name="a">\n')[1].split(
                b"\n</dependency>"
            )[0]
            bounds = {
                "max_input_bytes": len(prompt),
                "max_dependency_bytes": len(selected),
            }
            if edge.context != "summary":
                bounds["max_selection_bytes"] = (
                    len(source) if edge.context == "fields" else 5
                )
            for option, bound in bounds.items():
                with self.subTest(mode=edge.context, option=option):
                    self.assertTrue((await flow(**{option: bound}).run_async()).ok)
                    prompts.pop("b")
                    self.assertFalse((await flow(**{option: bound - 1}).run_async()).ok)
                    self.assertNotIn("b", prompts)

    async def test_mixed_selection_exact_complete_input_bound(self):
        prompts = {}
        source = b'{"keep":"value","omit":42}'

        async def runner(task, prompt):
            prompts[task.name] = prompt
            return tny.WorkflowTaskExecution(source, session_id=b"session")

        edges = [
            tny.WorkflowDependency("raw"),
            tny.WorkflowDependency("summary", context="summary", summary="é"),
            tny.WorkflowDependency("fields", context="fields", fields=("keep",)),
            tny.WorkflowDependency("artifact", context="artifact", offset=1, length=5),
            tny.WorkflowDependency("none", include_output=False),
        ]

        def flow(bound):
            w = tny.Workflow(runner=runner, max_input_bytes=bound)
            for edge in edges:
                w.task(edge.name, "p")
            return w.task("consumer", "é", depends_on=edges)

        self.assertTrue((await flow(10000).run_async()).ok)
        prompt = prompts.pop("consumer")
        self.assertIn(b'"data_base64":"ImtlZXA="', prompt)
        self.assertIn(b'"session_base64":"c2Vzc2lvbg=="', prompt)
        self.assertNotIn(b'<dependency name="none">', prompt)
        self.assertTrue((await flow(len(prompt)).run_async()).ok)
        prompts.pop("consumer")
        rejected = await flow(len(prompt) - 1).run_async()
        self.assertFalse(rejected.ok)
        self.assertIsNone(rejected["consumer"].error.__traceback__)
        self.assertNotIn("consumer", prompts)
