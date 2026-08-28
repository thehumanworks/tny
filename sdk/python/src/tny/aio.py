"""asyncio adapters that preserve libtny's single owner thread."""

from __future__ import annotations

import asyncio
import os
import warnings
import weakref
from collections.abc import AsyncIterator, Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from functools import partial
from typing import TYPE_CHECKING, TypeVar

from ._binding import Library
from .errors import BadStateError
from .events import AnyEvent, ErrorEvent, EventStreamError, PermissionRequestEvent
from .runtime import (
    CancellationToken,
    PermissionDecision,
    Runtime,
    RuntimeConfig,
    Session,
)

if TYPE_CHECKING:
    from .callbacks import AsyncCustomTool, CustomTool, HostServices, ToolRegistration

T = TypeVar("T")


async def _await_cancellation_immune(awaitable: asyncio.Future[T]) -> T:
    """Finish owner-thread cleanup even if the caller cancels repeatedly."""
    while True:
        try:
            return await asyncio.shield(awaitable)
        except asyncio.CancelledError:
            continue


def _finalize_owner(
    executor: ThreadPoolExecutor, runtime_holder: list[Runtime | None]
) -> None:
    warnings.warn(
        "unclosed tny.AsyncRuntime; scheduling owner-thread cleanup",
        ResourceWarning,
        stacklevel=2,
    )

    def close_on_owner() -> None:
        runtime = runtime_holder[0]
        if runtime is not None:
            runtime.close()
            runtime_holder[0] = None

    try:
        future = executor.submit(close_on_owner)
    except RuntimeError:
        return
    future.add_done_callback(
        lambda _future: executor.shutdown(wait=False, cancel_futures=True)
    )


class AsyncRuntime:
    """Async runtime whose native calls are serialized on one worker thread."""

    def __init__(
        self,
        config: RuntimeConfig,
        *,
        library: Library | None = None,
        library_path: str | os.PathLike[str] | None = None,
        host_services: HostServices | None = None,
    ) -> None:
        self._config = config
        self._library = library
        self._library_path = library_path
        self._host_services = host_services
        self._executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="libtny-owner"
        )
        self._lifecycle_lock = asyncio.Lock()
        self._closed = False
        self._executor_shutdown = False
        self._runtime: Runtime | None = None
        self._runtime_holder: list[Runtime | None] = [None]
        self._session: AsyncSession | None = None
        self._finalizer = weakref.finalize(
            self, _finalize_owner, self._executor, self._runtime_holder
        )

    async def _call(
        self, function: Callable[..., T], *args: object, **kwargs: object
    ) -> T:
        if self._executor_shutdown:
            raise BadStateError(-2)
        loop = asyncio.get_running_loop()
        call = partial(function, *args, **kwargs)
        return await loop.run_in_executor(self._executor, call)

    async def _open_locked(self) -> None:
        if self._closed:
            raise BadStateError(-2)
        if self._runtime is None:
            config = self._config
            library = self._library
            library_path = self._library_path
            host_services = self._host_services
            loop = asyncio.get_running_loop()
            creation = loop.run_in_executor(
                self._executor,
                partial(
                    Runtime,
                    config,
                    library=library,
                    library_path=library_path,
                    host_services=host_services,
                ),
            )
            try:
                self._runtime = await asyncio.shield(creation)
                self._runtime_holder[0] = self._runtime
                self._config = self._runtime.config
                self._host_services = None
            except asyncio.CancelledError:
                # Executor work cannot be cancelled once construction starts.
                # Reclaim and close its result on that owner thread before the
                # executor is detached from this wrapper.
                created: Runtime | None = None
                try:
                    resolved = await _await_cancellation_immune(creation)
                    created = resolved
                except BaseException:  # construction itself failed
                    pass
                if created is not None:
                    closing = loop.run_in_executor(self._executor, created.close)
                    await _await_cancellation_immune(closing)
                self._config = replace(config, api_key=b"")
                self._host_services = None
                self._closed = True
                self._finalizer.detach()
                self._executor.shutdown(wait=True, cancel_futures=True)
                self._executor_shutdown = True
                raise
            except BaseException:
                self._config = replace(config, api_key=b"")
                self._host_services = None
                self._closed = True
                self._finalizer.detach()
                self._executor.shutdown(wait=True, cancel_futures=True)
                self._executor_shutdown = True
                raise

    async def open(self) -> AsyncRuntime:
        async with self._lifecycle_lock:
            await self._open_locked()
        return self

    async def create_session(self) -> AsyncSession:
        async with self._lifecycle_lock:
            await self._open_locked()
            assert self._runtime is not None
            sync = await self._call(self._runtime.create_session)
            self._session = AsyncSession(self, sync)
            return self._session

    async def open_session(self, session_id: str | bytes) -> AsyncSession:
        async with self._lifecycle_lock:
            await self._open_locked()
            assert self._runtime is not None
            sync = await self._call(self._runtime.open_session, session_id)
            self._session = AsyncSession(self, sync)
            return self._session

    async def register_tool(
        self, tool: CustomTool | AsyncCustomTool
    ) -> AsyncToolRegistration:
        async with self._lifecycle_lock:
            await self._open_locked()
            assert self._runtime is not None
            loop = asyncio.get_running_loop()
            registration = await self._call(
                self._runtime.register_tool, tool, _loop=loop
            )
            return AsyncToolRegistration(self, registration)

    async def close(self) -> None:
        async with self._lifecycle_lock:
            if self._closed:
                self._config = replace(self._config, api_key=b"")
                self._host_services = None
                return
            if self._session is not None:
                await self._session.close()
            if self._runtime is not None:
                await self._call(self._runtime.close)
                self._runtime = None
                self._runtime_holder[0] = None
            self._closed = True
            self._config = replace(self._config, api_key=b"")
            self._host_services = None
            self._finalizer.detach()
            self._executor.shutdown(wait=True, cancel_futures=True)
            self._executor_shutdown = True

    async def __aenter__(self) -> AsyncRuntime:
        return await self.open()

    async def __aexit__(self, *_exc: object) -> None:
        await self.close()


class AsyncSession:
    def __init__(self, runtime: AsyncRuntime, sync: Session) -> None:
        self._runtime = runtime
        self._sync = sync
        self._token = CancellationToken()

    @property
    def closed(self) -> bool:
        return self._sync.closed

    async def id(self) -> bytes:
        return await self._runtime._call(lambda: self._sync.id)

    async def send(self, prompt: str | bytes) -> None:
        self._token = CancellationToken()
        await self._runtime._call(self._sync.send, prompt)

    async def steer(self, text: str | bytes) -> None:
        await self._runtime._call(self._sync.steer, text)

    async def respond_permission(
        self,
        request: PermissionRequestEvent | str | bytes,
        decision: PermissionDecision,
    ) -> None:
        await self._runtime._call(self._sync.respond_permission, request, decision)

    async def cancel(self) -> None:
        self._token.cancel()
        # ABI 1 cancel is cross-thread-safe and wakes an active native wait.
        # Session.cancel serializes against owner-thread close.
        self._sync.cancel()

    async def events(self, *, raise_on_error: bool = False) -> AsyncIterator[AnyEvent]:
        drained = False
        try:
            while True:

                def one_event() -> tuple[bool, AnyEvent | None]:
                    try:
                        return True, self._sync.next_event(
                            0.05, cancellation=self._token
                        )
                    except StopIteration:
                        return False, None

                has_event, event = await self._runtime._call(one_event)
                if not has_event:
                    drained = True
                    return
                if event is None:
                    continue
                if raise_on_error and isinstance(event, ErrorEvent):
                    raise EventStreamError(event)
                yield event
        finally:
            if not drained and not self.closed:
                await self._cancel_and_drain()

    async def run(
        self, prompt: str | bytes, *, raise_on_error: bool = False
    ) -> AsyncIterator[AnyEvent]:
        send_task = asyncio.create_task(self.send(prompt))
        try:
            await asyncio.shield(send_task)
            async for event in self.events(raise_on_error=raise_on_error):
                yield event
        finally:
            if not send_task.done():
                try:
                    await asyncio.wait_for(asyncio.shield(send_task), timeout=15.0)
                except BaseException:  # cleanup proceeds through close below
                    pass
            if self._sync._turn_active and not self.closed:
                await self._cancel_and_drain()

    async def _cancel_and_drain(self) -> None:
        self._token.cancel()
        try:
            self._sync.cancel()
        except BadStateError:
            pass
        deadline = asyncio.get_running_loop().time() + 5.0
        while not self.closed:

            def one_event() -> bool:
                try:
                    self._sync.next_event(0.1, cancellation=self._token)
                    return True
                except StopIteration:
                    return False
                except BadStateError:
                    return False

            if not await self._runtime._call(one_event):
                return
            if asyncio.get_running_loop().time() >= deadline:
                await self.close()
                return

    async def close(self) -> None:
        if not self._sync.closed:
            await self._runtime._call(self._sync.close)
        if self._runtime._session is self:
            self._runtime._session = None

    async def __aenter__(self) -> AsyncSession:
        return self

    async def __aexit__(self, *_exc: object) -> None:
        await self.close()


class AsyncToolRegistration:
    def __init__(self, runtime: AsyncRuntime, sync: ToolRegistration) -> None:
        self._runtime = runtime
        self._sync = sync

    @property
    def closed(self) -> bool:
        return self._sync.closed

    async def close(self) -> None:
        if not self._sync.closed:
            await self._runtime._call(self._sync.close)

    async def __aenter__(self) -> AsyncToolRegistration:
        if self.closed:
            raise BadStateError(-2)
        return self

    async def __aexit__(self, *_exc: object) -> None:
        await self.close()
