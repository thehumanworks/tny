"""Exercise real async session/generator ownership at workflow exit boundaries."""

import asyncio
import builtins
import gc
import sys
import threading
import unittest
import weakref
from collections import deque
from unittest.mock import patch

import tny


def usage(tokens):
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


def permission():
    return tny.PermissionRequestEvent(
        kind=4,
        schema_version=1,
        sequence=2,
        timestamp_ms=0,
        provider=b"fixture",
        session_id=b"session",
        turn_id=b"turn",
        type="permission_request",
        permission_id=b"permission",
        summary=b"fixture",
        options=tny.PermissionOptions.ALLOW | tny.PermissionOptions.DENY,
    )


class StreamCleanupTests(unittest.IsolatedAsyncioTestCase):
    async def test_real_session_drains_before_close_on_callback_exits(self):
        # Only the synchronous transport is replaced. AsyncRuntime's real owner
        # executor, AsyncSession, run() and events() generators remain in use.
        for callback in ("event", "permission"):
            for exit_mode in ("error", "cancel", "recancel", "invalid"):
                if callback == "event" and exit_mode == "invalid":
                    continue
                with self.subTest(callback=callback, exit_mode=exit_mode):
                    await self._exercise_exit(callback, exit_mode)

    async def _exercise_exit(self, callback, exit_mode):
        observed = []
        entered = asyncio.Event()
        draining = asyncio.Event()
        release = threading.Event()
        loop = asyncio.get_running_loop()
        events = deque([usage(7)])
        if callback == "permission":
            events.append(permission())
        events.append(usage(9))

        class SyncSession:
            id = b"session"
            closed = False
            _turn_active = False

            def send(self, prompt):
                self._turn_active = True

            def next_event(self, *args, **kwargs):
                if self.closed:
                    raise AssertionError("read after close")
                if not events:
                    self._turn_active = False
                    raise StopIteration
                event = events.popleft()
                if isinstance(event, tny.UsageEvent):
                    if event.input_tokens == 9 and exit_mode == "recancel":
                        loop.call_soon_threadsafe(draining.set)
                        if not release.wait(5):
                            raise AssertionError("cleanup barrier timed out")
                    observed.append(f"usage{event.input_tokens}")
                return event

            def cancel(self):
                if self.closed:
                    raise AssertionError("cancel after close")
                observed.append("cancel")

            def close(self):
                observed.append("close")
                self.closed = True
                self._turn_active = False

            def respond_permission(self, *args):
                raise AssertionError("failed callback cannot authorize permission")

        class SyncRuntime:
            def __init__(self, config, **kwargs):
                self.config = config

            def create_session(self):
                return SyncSession()

            def close(self):
                pass

        async def fail_or_wait(*args):
            entered.set()
            if exit_mode == "error":
                raise ValueError("callback diagnostic")
            if exit_mode == "invalid":
                return "not a permission decision"
            await asyncio.Event().wait()

        async def on_event(task, event):
            if isinstance(event, tny.UsageEvent):
                await fail_or_wait()

        workflow = tny.Workflow(
            tny.RuntimeConfig(workspace="."),
            on_event=on_event if callback == "event" else None,
            on_permission=fail_or_wait if callback == "permission" else None,
        ).task("a", "prompt")
        with patch("tny.aio.Runtime", SyncRuntime):
            running = asyncio.create_task(workflow.run_async())
            try:
                await asyncio.wait_for(entered.wait(), 5)
                before = workflow.partial_usage
                if exit_mode in ("cancel", "recancel"):
                    running.cancel()
                    if exit_mode == "recancel":
                        await asyncio.wait_for(draining.wait(), 5)
                        running.cancel()
                        await asyncio.sleep(0)
                        self.assertNotIn("close", observed)
                        release.set()
                    with self.assertRaises(asyncio.CancelledError):
                        await asyncio.wait_for(running, 5)
                else:
                    result = await asyncio.wait_for(running, 5)
                    self.assertFalse(result.ok)
                    self.assertEqual(result.usage["input_tokens"], 9)
                    if exit_mode == "error":
                        self.assertIsInstance(result["a"].error, ValueError)
                        self.assertEqual(str(result["a"].error), "callback diagnostic")
                    else:
                        self.assertIsInstance(result["a"].error, tny.WorkflowRunError)
                self.assertEqual(before["input_tokens"], 7)
                self.assertEqual(workflow.partial_usage["input_tokens"], 9)
                self.assertEqual(workflow.partial_usage["known_tasks"], 1)
                self.assertEqual(workflow.partial_usage["cost"], 0.25)
                gc.collect()
                await asyncio.sleep(0)
                self.assertEqual(observed, ["usage7", "cancel", "usage9", "close"])
            finally:
                release.set()
                if not running.done():
                    running.cancel()
                await asyncio.gather(running, return_exceptions=True)


class ExceptionOwnershipTests(unittest.IsolatedAsyncioTestCase):
    @unittest.skipIf(sys.version_info < (3, 11), "stdlib exception groups require 3.11")
    async def test_nested_and_mixed_groups_release_all_child_frames(self):
        class Sentinel:
            pass

        for returned in (False, True):
            with self.subTest(returned_base_group=returned):
                refs = []
                groups = {}
                members = {}

                def build_error(task, prompt):
                    local = Sentinel()
                    refs.append(weakref.ref(local))
                    try:
                        raise ValueError("leaf diagnostic")
                    except ValueError as leaf:
                        try:
                            raise TypeError("cause diagnostic")
                        except TypeError as cause:
                            try:
                                raise OSError("context diagnostic")
                            except OSError as context:
                                leaf.__cause__ = cause
                                leaf.__context__ = context
                                cause.__cause__ = leaf  # shared/cyclic chain
                                nested = builtins.ExceptionGroup(
                                    "nested", [leaf, context]
                                )
                                group = builtins.ExceptionGroup("outer", [leaf, nested])
                                children = [leaf, cause, context, nested]
                                if returned:
                                    try:
                                        raise KeyboardInterrupt("interrupt diagnostic")
                                    except KeyboardInterrupt as interrupt:
                                        group = builtins.BaseExceptionGroup(
                                            "base", [group, interrupt]
                                        )
                                        children.append(interrupt)
                                groups[task.name] = group
                                members[task.name] = (tuple(group.exceptions), children)
                                return group

                async def runner(task, prompt):
                    if task.name == "source":
                        return tny.WorkflowTaskExecution(b"x" * 262144)
                    group = build_error(task, prompt)
                    if returned:
                        return tny.WorkflowTaskExecution(b"", error=group)
                    # No enclosing except: only group membership owns the leaf
                    # tracebacks, not an incidental top-level context chain.
                    raise group

                workflow = tny.Workflow(runner=runner, max_concurrency=1).task(
                    "source", "p"
                )
                for index in range(32):
                    workflow.task(f"c{index}", "p", depends_on=["source"])
                result = await workflow.run_async()
                gc.collect()
                self.assertEqual(len(result.failed), 32)
                self.assertTrue(all(ref() is None for ref in refs))
                for failed in result.failed:
                    group = failed.error
                    self.assertIs(group, groups[failed.name])
                    topology, children = members[failed.name]
                    self.assertEqual(group.exceptions, topology)
                    self.assertEqual(group.message, "base" if returned else "outer")
                    self.assertEqual(
                        type(group),
                        builtins.BaseExceptionGroup
                        if returned
                        else builtins.ExceptionGroup,
                    )
                    for error in [group, *children]:
                        self.assertIsNone(error.__traceback__)
                        self.assertIsNone(error.__cause__)
                        self.assertIsNone(error.__context__)
                    self.assertIsInstance(children[0], ValueError)
                    self.assertEqual(str(children[0]), "leaf diagnostic")
                    self.assertIsInstance(children[1], TypeError)
                    self.assertEqual(str(children[1]), "cause diagnostic")
                    self.assertIsInstance(children[2], OSError)
                    self.assertEqual(str(children[2]), "context diagnostic")
                    nested = children[3]
                    self.assertEqual(nested.message, "nested")
                    self.assertEqual(nested.exceptions, (children[0], children[2]))

    async def test_ordinary_cause_and_context_are_detached_not_application_data(self):
        errors = []

        async def runner(task, prompt):
            try:
                raise ValueError("cause")
            except ValueError as cause:
                try:
                    raise LookupError("context")
                except LookupError as context:
                    outer = RuntimeError("ordinary")
                    outer.__cause__ = cause
                    outer.__context__ = context
                    cause.__context__ = outer
                    outer.application_data = ValueError("application-owned")
                    errors.extend([outer, cause, context])
                    return tny.WorkflowTaskExecution(b"", error=outer)

        result = await tny.Workflow(runner=runner).task("a", "p").run_async()
        self.assertIs(result["a"].error, errors[0])
        for error, message in zip(
            errors, ("ordinary", "cause", "context"), strict=True
        ):
            self.assertEqual(str(error), message)
            self.assertIsNone(error.__traceback__)
            self.assertIsNone(error.__cause__)
            self.assertIsNone(error.__context__)
        self.assertEqual(str(errors[0].application_data), "application-owned")
