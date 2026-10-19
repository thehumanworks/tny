#!/usr/bin/env python3
"""Persistent JSONL host for optional tny Python extensions.

Protocol stdout is reserved for one JSON response per input line.  Extension
imports, setup functions, and handlers run with stdout redirected to stderr so
an accidental print cannot corrupt the stream.
"""

import contextlib
import json
import sys
import traceback
from dataclasses import dataclass
from typing import Any, Dict, List, Mapping, TextIO, Tuple

from tny_ext.actions import action_to_dict
from tny_ext.capabilities import capability_view_from_dict
from tny_ext.events import event_from_dict
from tny_ext.loader import load_extension, run_awaitable

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


def _failure(
    kind: str, error: BaseException, debug: bool, **fields: Any
) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "kind": kind,
        "message": str(error) or type(error).__name__,
    }
    result.update(fields)
    if debug:
        rendered = "".join(
            traceback.format_exception(
                type(error), error, error.__traceback__, limit=12
            )
        )
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
        if not isinstance(entries_value, list) or not all(
            isinstance(item, str) for item in entries_value
        ):
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
                extension = load_extension(entry, ordinal, cwd, capabilities)
                name, path = extension.name, extension.entry
                for registration in extension.registrations:
                    handler_id = "%d:%s:%d" % (
                        ordinal,
                        registration.event,
                        registration.index,
                    )
                    loaded = LoadedHandler(
                        handler_id, registration.event, name, registration.handler
                    )
                    self.handlers[handler_id] = loaded
                    subscriptions.append(
                        {
                            "event": registration.event,
                            "handler_id": handler_id,
                            "extension": name,
                            "entry": path,
                        }
                    )
                extensions.append(
                    {
                        "entry": path,
                        "name": name,
                        "handlers": len(extension.registrations),
                    }
                )
            except BaseException as error:
                load_errors.append(
                    _failure("load_error", error, self.debug, extension=entry)
                )
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
            raise ValueError(
                "handler %s subscribes to %s, not %s"
                % (handler_id, loaded.event, event.type)
            )
        try:
            with contextlib.redirect_stdout(sys.stderr):
                result = run_awaitable(loaded.callback(event))
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
        self.protocol_out.write(
            json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n"
        )
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
            host.respond(
                request_id,
                {"error": _failure("protocol_error", error, host.debug)},
                False,
            )
        except BaseException as error:
            # A host bug should still be a bounded response instead of corrupting
            # or abruptly closing the JSONL stream.
            host.respond(
                request_id, {"error": _failure("host_error", error, host.debug)}, False
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
