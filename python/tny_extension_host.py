#!/usr/bin/env python3
"""Persistent JSONL host for optional tny Python extensions.

Protocol stdout is reserved for one JSON response per input line.  Extension
imports, setup functions, and handlers run with stdout redirected to stderr so
an accidental print cannot corrupt the stream.
"""

import asyncio
import contextlib
import importlib.util
import inspect
import json
import os
import sys
import traceback
from dataclasses import dataclass
from types import ModuleType
from typing import Any, Dict, List, Mapping, Optional, TextIO, Tuple

from tny_ext import ExtensionAPI
from tny_ext.actions import action_to_dict
from tny_ext.capabilities import capability_view_from_dict
from tny_ext.events import event_from_dict


PROTOCOL_VERSION = 1
EVENT_SCHEMA_VERSION = 1
ACTION_SCHEMA_VERSION = 1
CAPABILITY_SCHEMA_VERSION = 1
MAX_TRACEBACK_CHARS = 4096


@dataclass
class LoadedHandler:
    handler_id: str
    event: str
    extension: str
    callback: Any


def _entry_path(entry: str, cwd: Optional[str]) -> Tuple[str, bool]:
    expanded = os.path.expanduser(entry)
    if not os.path.isabs(expanded):
        expanded = os.path.join(cwd or os.getcwd(), expanded)
    resolved = os.path.abspath(expanded)
    if os.path.isdir(resolved):
        index = os.path.join(resolved, "index.py")
        if not os.path.isfile(index):
            raise FileNotFoundError("extension directory has no index.py: %s" % resolved)
        return index, True
    if not os.path.isfile(resolved):
        raise FileNotFoundError("extension entry does not exist: %s" % resolved)
    # The C discovery layer passes the resolved index.py path, not its parent
    # directory.  Treat it as a package so relative imports keep working and
    # the extension name remains the directory name.
    return resolved, os.path.basename(resolved) == "index.py"


@contextlib.contextmanager
def _temporary_sys_path(path: str) -> Any:
    sys.path.insert(0, path)
    try:
        yield
    finally:
        try:
            sys.path.remove(path)
        except ValueError:
            pass


def _load_module(entry: str, ordinal: int, cwd: Optional[str]) -> Tuple[ModuleType, str, str]:
    path, is_package = _entry_path(entry, cwd)
    extension_dir = os.path.dirname(path)
    extension_name = os.path.basename(extension_dir) if is_package else os.path.splitext(os.path.basename(path))[0]
    module_name = "_tny_extension_%d_%s" % (ordinal, extension_name.replace("-", "_"))
    if is_package:
        spec = importlib.util.spec_from_file_location(module_name, path, submodule_search_locations=[extension_dir])
    else:
        spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise ImportError("cannot create import spec for extension: %s" % path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        with _temporary_sys_path(extension_dir):
            spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(module_name, None)
        raise
    return module, extension_name, path


def _run_awaitable(value: Any) -> Any:
    if inspect.isawaitable(value):
        return asyncio.run(value)
    return value


def _failure(kind: str, error: BaseException, debug: bool, **fields: Any) -> Dict[str, Any]:
    result: Dict[str, Any] = {"kind": kind, "message": str(error) or type(error).__name__}
    result.update(fields)
    if debug:
        rendered = "".join(traceback.format_exception(type(error), error, error.__traceback__, limit=12))
        if len(rendered) > MAX_TRACEBACK_CHARS:
            rendered = rendered[-MAX_TRACEBACK_CHARS:]
        result["traceback"] = rendered
    return result


class ExtensionHost:
    def __init__(self, protocol_out: TextIO) -> None:
        self.protocol_out = protocol_out
        self.handlers: Dict[str, LoadedHandler] = {}
        self.initialized = False
        self.debug = False

    def initialize(self, request: Mapping[str, Any]) -> Dict[str, Any]:
        if self.initialized:
            raise ValueError("extension host is already initialized")
        entries_value = request.get("entries")
        if entries_value is None and isinstance(request.get("entry"), str):
            entries_value = [request.get("entry")]
        if not isinstance(entries_value, list) or not all(isinstance(item, str) for item in entries_value):
            raise ValueError("initialize.entries must be an array of paths")
        cwd = request.get("cwd")
        if cwd is not None and not isinstance(cwd, str):
            raise ValueError("initialize.cwd must be a string")
        schema_value = request.get("schema")
        if schema_value is not None:
            if not isinstance(schema_value, Mapping):
                raise ValueError("initialize.schema must be an object")
            expected = {
                "events": EVENT_SCHEMA_VERSION,
                "actions": ACTION_SCHEMA_VERSION,
                "capabilities": CAPABILITY_SCHEMA_VERSION,
            }
            for name, supported in expected.items():
                requested = schema_value.get(name, supported)
                if not isinstance(requested, int) or isinstance(requested, bool):
                    raise ValueError("initialize.schema.%s must be an integer" % name)
                if requested != supported:
                    raise ValueError(
                        "unsupported %s schema major %s (host supports %s)"
                        % (name, requested, supported)
                    )
        capabilities = capability_view_from_dict(request.get("capabilities"))
        if capabilities.schema_version != CAPABILITY_SCHEMA_VERSION:
            raise ValueError(
                "unsupported capabilities schema major %s (host supports %s)"
                % (capabilities.schema_version, CAPABILITY_SCHEMA_VERSION)
            )
        self.debug = request.get("debug") is True
        subscriptions: List[Dict[str, str]] = []
        extensions: List[Dict[str, Any]] = []
        load_errors: List[Dict[str, Any]] = []
        for ordinal, entry in enumerate(entries_value):
            try:
                with contextlib.redirect_stdout(sys.stderr):
                    module, name, path = _load_module(entry, ordinal, cwd)
                    setup = getattr(module, "setup", None)
                    if not callable(setup):
                        raise TypeError("extension must define callable setup(api): %s" % path)
                    api = ExtensionAPI(name, capabilities)
                    _run_awaitable(setup(api))
                for registration in api.registrations:
                    handler_id = "%d:%s:%d" % (ordinal, registration.event, registration.index)
                    loaded = LoadedHandler(handler_id, registration.event, name, registration.handler)
                    self.handlers[handler_id] = loaded
                    subscriptions.append(
                        {
                            "event": registration.event,
                            "handler_id": handler_id,
                            "extension": name,
                            "entry": path,
                        }
                    )
                extensions.append({"entry": path, "name": name, "handlers": len(api.registrations)})
            except BaseException as error:
                load_errors.append(_failure("load_error", error, self.debug, extension=entry))
        self.initialized = True
        return {
            "protocol": PROTOCOL_VERSION,
            "schema": {
                "events": EVENT_SCHEMA_VERSION,
                "actions": ACTION_SCHEMA_VERSION,
                "capabilities": CAPABILITY_SCHEMA_VERSION,
            },
            "subscriptions": subscriptions,
            "extensions": extensions,
            "load_errors": load_errors,
        }

    def invoke(self, request: Mapping[str, Any]) -> Dict[str, Any]:
        if not self.initialized:
            raise ValueError("extension host is not initialized")
        handler_id = request.get("handler_id")
        if not isinstance(handler_id, str) or handler_id not in self.handlers:
            raise ValueError("unknown handler_id: %r" % (handler_id,))
        event_value = request.get("event")
        if not isinstance(event_value, Mapping):
            raise ValueError("invoke.event must be an object")
        loaded = self.handlers[handler_id]
        event = event_from_dict(event_value)
        if loaded.event != "*" and event.type != loaded.event:
            raise ValueError("handler %s subscribes to %s, not %s" % (handler_id, loaded.event, event.type))
        try:
            with contextlib.redirect_stdout(sys.stderr):
                result = _run_awaitable(loaded.callback(event))
            return {"action": action_to_dict(result)}
        except BaseException as error:
            detail = _failure(
                "extension_error",
                error,
                self.debug,
                extension=loaded.extension,
                event=event.type,
                handler_id=handler_id,
            )
            failure: Dict[str, Any] = {
                "code": "handler_exception",
                "message": detail["message"],
            }
            if "traceback" in detail:
                failure["traceback"] = detail["traceback"]
            return {
                "error": detail,
                "failure": failure,
            }

    def dispatch(self, request: Mapping[str, Any]) -> Tuple[Dict[str, Any], bool]:
        op = request.get("op")
        if op == "initialize":
            return self.initialize(request), False
        if op == "invoke":
            result = self.invoke(request)
            return result, False
        if op == "ping":
            return {"protocol": PROTOCOL_VERSION}, False
        if op == "shutdown":
            return {}, True
        raise ValueError("unknown op: %r" % (op,))

    def respond(self, request_id: Any, body: Mapping[str, Any], ok: bool) -> None:
        response: Dict[str, Any] = {"id": request_id, "ok": ok}
        response.update(body)
        self.protocol_out.write(json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n")
        self.protocol_out.flush()


def main() -> int:
    protocol_out = sys.stdout
    host = ExtensionHost(protocol_out)
    for raw_line in sys.stdin:
        if not raw_line.strip():
            continue
        request_id: Any = None
        try:
            request = json.loads(raw_line)
            if not isinstance(request, Mapping):
                raise ValueError("request must be a JSON object")
            request_id = request.get("id")
            body, should_exit = host.dispatch(request)
            if "error" in body:
                host.respond(request_id, body, False)
            else:
                host.respond(request_id, body, True)
            if should_exit:
                return 0
        except (ValueError, TypeError, json.JSONDecodeError) as error:
            host.respond(request_id, {"error": _failure("protocol_error", error, host.debug)}, False)
        except BaseException as error:
            # A host bug should still be a bounded response instead of corrupting
            # or abruptly closing the JSONL stream.
            host.respond(request_id, {"error": _failure("host_error", error, host.debug)}, False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
