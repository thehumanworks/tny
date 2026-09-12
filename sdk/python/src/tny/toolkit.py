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
from .errors import InvalidArgumentError, ProtocolError, TnyError, UnsupportedError
from .runtime import CancellationToken

ImageQuality = Literal["auto", "low", "medium", "high", "xhigh", "max"]
ImageSizeStatus = Literal["auto", "match", "mismatch", "unverifiable", "unsupported"]
ImageOperation = Literal["generate", "edit"]
PathLike = str | os.PathLike[str]
Text = str | bytes

# The exact codes libtny assigns locally for --strict-size; a provider string
# can never select this path (docs/images.md).
STRICT_IMAGE_CODES = frozenset(
    {
        "IMAGE_STRICT_SIZE_INVALID",
        "IMAGE_SIZE_MISMATCH",
        "IMAGE_SIZE_UNVERIFIABLE",
        "IMAGE_SIZE_UNSUPPORTED",
    }
)
# The one failure that keeps a written file: the image was committed and only
# its manifest could not be finalized (docs/images.md, ADR 0095).
RETAINED_IMAGE_CODE = "IMAGE_MANIFEST_FINALIZE_FAILED"
_SIZE_STATUSES = frozenset({"auto", "match", "mismatch", "unverifiable", "unsupported"})
_DETAIL_FIELDS = frozenset(
    {
        "kind",
        "ok",
        "operation",
        "code",
        "error",
        "mime_type",
        "requested_size",
        "effective_size",
        "width",
        "height",
        "size_status",
        "path",
        "committed",
    }
)
_RETAINED_FIELDS = _DETAIL_FIELDS | {"bytes", "operation_id", "manifest_path"}


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
    """Dimensions are read from the returned bytes, never from the request.

    ``width``/``height`` are ``None`` when the header could not be read, and
    ``size_status`` says why. ``manifest_path`` is ``None`` when persistence
    was declined, and ``seed``/``request_id`` are ``None`` unless the provider
    actually supplied them. A libtny older than this metadata leaves the new
    fields ``None`` rather than inventing a value.
    """

    path: Path
    mime_type: str
    byte_count: int
    provider: str
    model: str
    requested_size: str | None = None
    effective_size: str | None = None
    width: int | None = None
    height: int | None = None
    size_status: ImageSizeStatus | None = None
    operation_id: str | None = None
    manifest_path: Path | None = None
    seed: int | None = None
    request_id: str | None = None


@dataclass(frozen=True, slots=True)
class ImageFailureDetail:
    """Why a ``strict_size`` request was rejected, and what was not written.

    This is a failure type, never an :class:`ImageResult`: no file exists, so
    ``path`` is always ``None`` and ``committed`` always ``False``. Every value
    is decided locally — the requested size, the literal actually sent (``None``
    when nothing was), and dimensions/MIME read from returned bytes. It carries
    no credential, endpoint, prompt, reference path or provider response text.
    """

    code: str
    operation: ImageOperation
    message: str
    requested_size: str
    effective_size: str | None
    width: int | None
    height: int | None
    size_status: ImageSizeStatus
    mime_type: str | None
    path: None = None
    committed: Literal[False] = field(default=False, init=False)


@dataclass(frozen=True, slots=True)
class RetainedImageDetail:
    """The generated image was written and kept; only its record failed.

    This is the one failure that names a file, so ``committed`` is always
    ``True`` and ``path`` is the artifact tny deliberately did not delete. The
    values are the same locally decided metadata as a successful result minus
    anything the provider said: no credential, endpoint, prompt, reference path
    or provider response text. It is never an :class:`ImageResult`, and never
    parsed as an :class:`ImageFailureDetail`.
    """

    code: str
    operation: ImageOperation
    message: str
    path: Path
    byte_count: int
    requested_size: str
    effective_size: str | None
    width: int | None
    height: int | None
    size_status: ImageSizeStatus
    mime_type: str | None
    operation_id: str | None
    manifest_path: Path | None
    committed: Literal[True] = field(default=True, init=False)


# What ``TnyError.image_detail`` may hold: the two shapes are discriminated by
# ``committed`` and by the code, never merged into one permissive reader.
ImageDetail = ImageFailureDetail | RetainedImageDetail


def _text(value: object, *, optional: bool = False) -> bool:
    if value is None:
        return optional
    return isinstance(value, str) and bool(value)


def _dimension(value: object) -> bool:
    return value is None or (
        isinstance(value, int) and not isinstance(value, bool) and value > 0
    )


def _shared_detail_fields(value: dict[str, Any]) -> bool:
    """The metadata both shapes carry, all of it locally decided."""
    return (
        value["kind"] == "image"
        and value["ok"] is False
        and value["operation"] in ("generate", "edit")
        and value["size_status"] in _SIZE_STATUSES
        and _text(value["error"])
        and _text(value["requested_size"])
        and _text(value["effective_size"], optional=True)
        and _text(value["mime_type"], optional=True)
        and _dimension(value["width"])
        and _dimension(value["height"])
    )


def _retained_image_detail(value: dict[str, Any]) -> RetainedImageDetail | None:
    """Accept only a complete retained-artifact object: committed, with a path."""
    if not _RETAINED_FIELDS <= value.keys():
        return None
    manifest = value["manifest_path"]
    if (
        value["code"] != RETAINED_IMAGE_CODE
        or value["committed"] is not True
        or not _text(value["path"])
        or not isinstance(value["bytes"], int)
        or isinstance(value["bytes"], bool)
        or value["bytes"] < 0
        or not _text(value["operation_id"], optional=True)
        or not _text(manifest, optional=True)
        or not _shared_detail_fields(value)
    ):
        return None
    return RetainedImageDetail(
        value["code"],
        value["operation"],
        value["error"],
        Path(value["path"]),
        value["bytes"],
        value["requested_size"],
        value["effective_size"],
        value["width"],
        value["height"],
        value["size_status"],
        value["mime_type"],
        value["operation_id"],
        Path(manifest) if manifest is not None else None,
    )


def _image_failure_detail(raw: bytes) -> ImageDetail | None:
    """Accept only a complete, locally shaped image-failure object.

    The two shapes are discriminated first: a retained artifact reports
    ``committed`` true with its path, a strict-size rejection reports neither.
    """
    if not raw:
        return None
    try:
        value = json.loads(raw)
    except ValueError:
        return None
    if not isinstance(value, dict) or not _DETAIL_FIELDS <= value.keys():
        return None
    if value["code"] == RETAINED_IMAGE_CODE or value["committed"] is not False:
        return _retained_image_detail(value)
    if (
        value["path"] is not None
        or value["code"] not in STRICT_IMAGE_CODES
        or not _shared_detail_fields(value)
    ):
        return None
    return ImageFailureDetail(
        value["code"],
        value["operation"],
        value["error"],
        value["requested_size"],
        value["effective_size"],
        value["width"],
        value["height"],
        value["size_status"],
        value["mime_type"],
    )


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
    manifest = result.get("manifest_path")
    return ImageResult(
        Path(result["path"]),
        result["mime_type"],
        result["bytes"],
        result["provider"],
        result["model"],
        result.get("requested_size"),
        result.get("effective_size"),
        result.get("width"),
        result.get("height"),
        result.get("size_status"),
        result.get("operation_id"),
        Path(manifest) if manifest is not None else None,
        result.get("seed"),
        result.get("request_id"),
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
                # Copy the completed result before raise_status frees the
                # native error, and long before the job is destroyed below.
                detail = _image_failure_detail(
                    copy_bytes(ffi, lib.tny_toolkit_job_result(job[0]))
                )
                try:
                    self._library.raise_status(status, error[0])
                except TnyError as failure:
                    failure._image_detail = detail
                    raise
                raise ProtocolError(-10)  # raise_status never returns
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
        prompt: Text | None = None,
        *,
        output_file: PathLike,
        provider: str | None = None,
        model: str | None = None,
        quality: ImageQuality | None = None,
        size: str | None = None,
        strict_size: bool | None = None,
        persist_manifest: bool | None = None,
        from_manifest: PathLike | None = None,
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
                    "strict_size": strict_size,
                    "persist_manifest": persist_manifest,
                    "from_manifest": from_manifest,
                },
                cancellation,
            )
        )

    def edit_image(
        self,
        prompt: Text | None = None,
        *,
        images: Sequence[PathLike] = (),
        artifact: PathLike | None = None,
        output_file: PathLike,
        provider: str | None = None,
        model: str | None = None,
        quality: ImageQuality | None = None,
        size: str | None = None,
        strict_size: bool | None = None,
        persist_manifest: bool | None = None,
        from_manifest: PathLike | None = None,
        cancellation: CancellationToken | None = None,
    ) -> ImageResult:
        return _image(
            self._run(
                "edit_image",
                {
                    "prompt": prompt,
                    "images": list(images) or None,
                    "artifact": artifact,
                    "output_file": output_file,
                    "provider": provider,
                    "model": model,
                    "quality": quality,
                    "size": size,
                    "strict_size": strict_size,
                    "persist_manifest": persist_manifest,
                    "from_manifest": from_manifest,
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
        prompt: Text | None = None,
        *,
        output_file: PathLike,
        provider: str | None = None,
        model: str | None = None,
        quality: ImageQuality | None = None,
        size: str | None = None,
        strict_size: bool | None = None,
        persist_manifest: bool | None = None,
        from_manifest: PathLike | None = None,
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
                    "strict_size": strict_size,
                    "persist_manifest": persist_manifest,
                    "from_manifest": from_manifest,
                },
                cancellation,
            )
        )

    async def edit_image(
        self,
        prompt: Text | None = None,
        *,
        images: Sequence[PathLike] = (),
        artifact: PathLike | None = None,
        output_file: PathLike,
        provider: str | None = None,
        model: str | None = None,
        quality: ImageQuality | None = None,
        size: str | None = None,
        strict_size: bool | None = None,
        persist_manifest: bool | None = None,
        from_manifest: PathLike | None = None,
        cancellation: CancellationToken | None = None,
    ) -> ImageResult:
        return _image(
            await self._run(
                "edit_image",
                {
                    "prompt": prompt,
                    "images": list(images) or None,
                    "artifact": artifact,
                    "output_file": output_file,
                    "provider": provider,
                    "model": model,
                    "quality": quality,
                    "size": size,
                    "strict_size": strict_size,
                    "persist_manifest": persist_manifest,
                    "from_manifest": from_manifest,
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
