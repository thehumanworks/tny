"""Standalone native image, audio, and prompt services (libtny ABI 1.2+)."""

from __future__ import annotations

import asyncio
import json
import os
from collections.abc import Sequence
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Literal

from ._binding import Library, borrowed, copy_bytes
from .errors import InvalidArgumentError, ProtocolError, UnsupportedError
from .runtime import CancellationToken

ImageQuality = Literal["auto", "low", "medium", "high", "xhigh", "max"]
PathLike = str | os.PathLike[str]
Text = str | bytes


@dataclass(frozen=True, slots=True)
class ToolkitConfig:
    """Independent toolkit configuration; omitted credentials use tny login.

    Paths in requests resolve against the captured workspace. The optional
    settings file supplies provider profiles and optimisation defaults only;
    it never enables agent extensions, MCP, or persistent sessions.
    """

    workspace: PathLike = "."
    settings_path: PathLike | None = None
    chatgpt_token: Text | None = field(default=None, repr=False)
    chatgpt_account_id: Text | None = field(default=None, repr=False)
    codex_base_url: str | None = field(default=None, repr=False)
    xai_api_key: Text | None = field(default=None, repr=False)


@dataclass(frozen=True, slots=True)
class ImageResult:
    path: Path
    mime_type: str
    byte_count: int
    provider: str
    model: str


@dataclass(frozen=True, slots=True)
class SpeechResult:
    path: Path | None
    played: bool
    provider: str
    voice: str
    mime_type: str


@dataclass(frozen=True, slots=True)
class TranscriptionResult:
    text: bytes = field(repr=False)
    provider: str


@dataclass(frozen=True, slots=True)
class OptimisationResult:
    text: bytes = field(repr=False)
    provider: str
    model: str


def _value(value: object) -> object:
    if isinstance(value, bytes):
        return value.decode("utf-8")
    if isinstance(value, os.PathLike):
        return os.fspath(value)
    if isinstance(value, (list, tuple)):
        return [_value(item) for item in value]
    return value


def _image(result: dict[str, Any]) -> ImageResult:
    return ImageResult(
        Path(result["path"]),
        result["mime_type"],
        result["bytes"],
        result["provider"],
        result["model"],
    )


def _speech(result: dict[str, Any]) -> SpeechResult:
    return SpeechResult(
        Path(result["path"]) if result["path"] is not None else None,
        result["played"],
        result["provider"],
        result["voice"],
        result["mime_type"],
    )


def _transcription(result: dict[str, Any]) -> TranscriptionResult:
    return TranscriptionResult(result["text"].encode("utf-8"), result["provider"])


def _optimisation(result: dict[str, Any]) -> OptimisationResult:
    return OptimisationResult(
        result["text"].encode("utf-8"),
        result["provider"],
        result["model"],
    )


class Toolkit:
    """Synchronous toolkit with one independent native job per call.

    No Runtime, Session, CLI executable, or disposal is required. Concurrent
    calls share no native handles. Pass a CancellationToken to interrupt from
    another thread. The async adapter also joins cancelled native work.
    """

    def __init__(
        self,
        config: ToolkitConfig | None = None,
        *,
        library: Library | PathLike | None = None,
    ) -> None:
        config = config if config is not None else ToolkitConfig()
        if not isinstance(config, ToolkitConfig):
            raise InvalidArgumentError(-1)
        self._config = replace(config, workspace=os.path.abspath(config.workspace))
        self._library = library if isinstance(library, Library) else Library(library)
        if self._library.abi_minor < 2:
            raise UnsupportedError(-9, b"Toolkit requires libtny ABI 1.2 or newer")
        try:
            for name in ("create", "run", "cancel", "result", "destroy"):
                getattr(self._library.native, f"tny_toolkit_job_{name}")
        except AttributeError:
            raise UnsupportedError(-9, b"libtny is missing toolkit symbols") from None

    def __repr__(self) -> str:
        return f"Toolkit(workspace={os.fspath(self._config.workspace)!r})"

    def _run(
        self,
        operation: str,
        request: dict[str, object],
        cancellation: CancellationToken | None,
    ) -> dict[str, Any]:
        config = self._config
        values = {
            "workspace": config.workspace,
            "settings_path": config.settings_path,
            "chatgpt_token": config.chatgpt_token,
            "chatgpt_account_id": config.chatgpt_account_id,
            "codex_base_url": config.codex_base_url,
            "xai_api_key": config.xai_api_key,
        }
        try:
            payload = json.dumps(
                {
                    "version": 1,
                    "operation": operation,
                    "config": {
                        k: _value(v) for k, v in values.items() if v is not None
                    },
                    "request": {
                        k: _value(v) for k, v in request.items() if v is not None
                    },
                },
                ensure_ascii=False,
                allow_nan=False,
            ).encode("utf-8")
        except (TypeError, ValueError, UnicodeError):
            raise InvalidArgumentError(-1) from None
        lib, ffi = self._library.native, self._library.ffi
        job = ffi.new("tny_toolkit_job **")
        error = ffi.new("tny_error **")
        buffer, view = borrowed(ffi, payload)
        try:
            status = lib.tny_toolkit_job_create(view[0], job, error)
        finally:
            ffi.buffer(buffer, len(payload))[:] = b"\0" * len(payload)
        if status:
            self._library.raise_status(status, error[0])
        unsubscribe = None
        try:
            if cancellation is not None:
                unsubscribe = cancellation._subscribe(
                    lambda: lib.tny_toolkit_job_cancel(job[0])
                )
            status = lib.tny_toolkit_job_run(job[0], error)
            if status:
                self._library.raise_status(status, error[0])
            data = json.loads(copy_bytes(ffi, lib.tny_toolkit_job_result(job[0])))
            if not isinstance(data, dict):
                raise ProtocolError(-10)
            return data
        finally:
            if unsubscribe is not None:
                unsubscribe()
            lib.tny_toolkit_job_destroy(job)

    def generate_image(
        self,
        prompt: Text,
        *,
        output_file: PathLike,
        provider: str | None = None,
        model: str | None = None,
        quality: ImageQuality | None = None,
        size: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> ImageResult:
        return _image(
            self._run(
                "generate_image",
                {
                    "prompt": prompt,
                    "output_file": output_file,
                    "provider": provider,
                    "model": model,
                    "quality": quality,
                    "size": size,
                },
                cancellation,
            )
        )

    def edit_image(
        self,
        prompt: Text,
        *,
        images: Sequence[PathLike],
        output_file: PathLike,
        provider: str | None = None,
        model: str | None = None,
        quality: ImageQuality | None = None,
        size: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> ImageResult:
        return _image(
            self._run(
                "edit_image",
                {
                    "prompt": prompt,
                    "images": list(images),
                    "output_file": output_file,
                    "provider": provider,
                    "model": model,
                    "quality": quality,
                    "size": size,
                },
                cancellation,
            )
        )

    def speak(
        self,
        text: Text,
        *,
        output_file: PathLike | None = None,
        provider: str | None = None,
        voice: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> SpeechResult:
        return _speech(
            self._run(
                "speak",
                {
                    "text": text,
                    "output_file": output_file,
                    "provider": provider,
                    "voice": voice,
                },
                cancellation,
            )
        )

    def transcribe(
        self,
        input_file: PathLike,
        *,
        provider: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> TranscriptionResult:
        return _transcription(
            self._run(
                "transcribe",
                {
                    "input_file": input_file,
                    "provider": provider,
                },
                cancellation,
            )
        )

    def dictate(
        self,
        *,
        seconds: int,
        provider: str | None = None,
        device: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> TranscriptionResult:
        return _transcription(
            self._run(
                "dictate",
                {
                    "seconds": seconds,
                    "provider": provider,
                    "device": device,
                },
                cancellation,
            )
        )

    def optimise(
        self,
        text: Text,
        *,
        provider: str | None = None,
        model: str | None = None,
        base_url: str | None = None,
        api_key: Text | None = None,
        wire_api: Literal["chat", "responses"] | None = None,
        timeout_seconds: int | None = None,
        cancellation: CancellationToken | None = None,
    ) -> OptimisationResult:
        return _optimisation(
            self._run(
                "optimise",
                {
                    "text": text,
                    "provider": provider,
                    "model": model,
                    "base_url": base_url,
                    "api_key": api_key,
                    "wire_api": wire_api,
                    "timeout_seconds": timeout_seconds,
                },
                cancellation,
            )
        )

    optimize = optimise


class AsyncToolkit:
    """Async toolkit; cancellation waits for native cleanup before propagating."""

    def __init__(
        self,
        config: ToolkitConfig | None = None,
        *,
        library: Library | PathLike | None = None,
    ) -> None:
        self._sync = Toolkit(config, library=library)

    def __repr__(self) -> str:
        return "Async" + repr(self._sync)

    async def _run(
        self,
        operation: str,
        request: dict[str, object],
        cancellation: CancellationToken | None,
    ) -> dict[str, Any]:
        token = CancellationToken()
        unsubscribe = (
            cancellation._subscribe(token.cancel) if cancellation is not None else None
        )
        task = asyncio.create_task(
            asyncio.to_thread(self._sync._run, operation, request, token)
        )
        try:
            await asyncio.wait({task})
            return task.result()
        except asyncio.CancelledError:
            token.cancel()
            # Keep the native buffers/job alive until the worker has observed
            # cancellation, including repeated Task.cancel() calls while joining.
            while not task.done():
                try:
                    await asyncio.wait({task})
                except asyncio.CancelledError:
                    continue
                except Exception:
                    break
            if task.done() and not task.cancelled():
                task.exception()
            raise
        finally:
            if unsubscribe is not None:
                unsubscribe()

    async def generate_image(
        self,
        prompt: Text,
        *,
        output_file: PathLike,
        provider: str | None = None,
        model: str | None = None,
        quality: ImageQuality | None = None,
        size: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> ImageResult:
        return _image(
            await self._run(
                "generate_image",
                {
                    "prompt": prompt,
                    "output_file": output_file,
                    "provider": provider,
                    "model": model,
                    "quality": quality,
                    "size": size,
                },
                cancellation,
            )
        )

    async def edit_image(
        self,
        prompt: Text,
        *,
        images: Sequence[PathLike],
        output_file: PathLike,
        provider: str | None = None,
        model: str | None = None,
        quality: ImageQuality | None = None,
        size: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> ImageResult:
        return _image(
            await self._run(
                "edit_image",
                {
                    "prompt": prompt,
                    "images": list(images),
                    "output_file": output_file,
                    "provider": provider,
                    "model": model,
                    "quality": quality,
                    "size": size,
                },
                cancellation,
            )
        )

    async def speak(
        self,
        text: Text,
        *,
        output_file: PathLike | None = None,
        provider: str | None = None,
        voice: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> SpeechResult:
        return _speech(
            await self._run(
                "speak",
                {
                    "text": text,
                    "output_file": output_file,
                    "provider": provider,
                    "voice": voice,
                },
                cancellation,
            )
        )

    async def transcribe(
        self,
        input_file: PathLike,
        *,
        provider: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> TranscriptionResult:
        return _transcription(
            await self._run(
                "transcribe",
                {
                    "input_file": input_file,
                    "provider": provider,
                },
                cancellation,
            )
        )

    async def dictate(
        self,
        *,
        seconds: int,
        provider: str | None = None,
        device: str | None = None,
        cancellation: CancellationToken | None = None,
    ) -> TranscriptionResult:
        return _transcription(
            await self._run(
                "dictate",
                {
                    "seconds": seconds,
                    "provider": provider,
                    "device": device,
                },
                cancellation,
            )
        )

    async def optimise(
        self,
        text: Text,
        *,
        provider: str | None = None,
        model: str | None = None,
        base_url: str | None = None,
        api_key: Text | None = None,
        wire_api: Literal["chat", "responses"] | None = None,
        timeout_seconds: int | None = None,
        cancellation: CancellationToken | None = None,
    ) -> OptimisationResult:
        return _optimisation(
            await self._run(
                "optimise",
                {
                    "text": text,
                    "provider": provider,
                    "model": model,
                    "base_url": base_url,
                    "api_key": api_key,
                    "wire_api": wire_api,
                    "timeout_seconds": timeout_seconds,
                },
                cancellation,
            )
        )

    optimize = optimise
