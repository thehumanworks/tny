#!/usr/bin/env python3
"""The JSONL line vocabulary is the public one, not a second dialect.

This is a registry-parity guard: it proves the serializer names and numbers
the same events as sdk/schema/events.json, include/tny/tny.h and the shipped
language bindings, and that the numeric binding is compile-checked rather
than retyped. Behavioral proof that the values are right lives in
tests/integration/test_ask_events_conformance.py, which compares real output
with the libtny readers.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = json.loads((ROOT / "sdk/schema/events.json").read_text())
SOURCE = (ROOT / "src/core/event_jsonl.c").read_text()
HEADER = (ROOT / "include/tny/tny.h").read_text()

TYPES = [event["type"] for event in SCHEMA["events"]]
FIELDS = {event["type"]: event["fields"] for event in SCHEMA["events"]}
# The private enum names the serializer switches on, in registry order.
PRIVATE = [event["c"].replace("TNY_EVENT_", "TNY_EV_") for event in SCHEMA["events"]]


class EventJsonlRegistryTest(unittest.TestCase):
    def test_type_names_match_the_registry(self) -> None:
        table = re.findall(r'case (TNY_EV_[A-Z_]+): return "([a-z_]+)";', SOURCE)
        self.assertEqual([name for name, _ in table], PRIVATE)
        self.assertEqual([value for _, value in table], TYPES)

    def emits(self, key: str) -> bool:
        """The key is written either as a JSON literal (\\"key\\":) or through
        the named-field helper (field_str(..., "key", ...))."""
        return f'\\"{key}\\":' in SOURCE or f'"{key}",' in SOURCE

    def test_every_payload_field_name_is_emitted(self) -> None:
        missing = [
            f"{event_type}.{field}"
            for event_type, fields in FIELDS.items()
            for field in fields
            if not self.emits(field)
        ]
        self.assertEqual(missing, [], f"fields the serializer never writes: {missing}")

    def test_envelope_keys_are_emitted(self) -> None:
        missing = [key for key in SCHEMA["envelope"] if not self.emits(key)]
        self.assertEqual(
            missing, [], f"envelope keys the serializer never writes: {missing}"
        )

    def test_numeric_vocabulary_is_compile_checked(self) -> None:
        asserts = re.findall(r"_Static_assert\(([A-Z_0-9]+) ==", SOURCE)
        for event in SCHEMA["events"]:
            self.assertIn(
                event["c"], asserts, f"{event['c']} is not pinned to the private enum"
            )
        for reason in (
            "TNY_STOP_REASON_DONE",
            "TNY_STOP_REASON_INTERRUPTED",
            "TNY_STOP_REASON_DENIED",
            "TNY_STOP_REASON_STEP_LIMIT",
            "TNY_STOP_REASON_ERROR",
        ):
            self.assertIn(reason, asserts, f"{reason} is not pinned")
            self.assertIn(reason, HEADER)

    def test_stop_reasons_stay_numeric(self) -> None:
        """The frozen ABI reports a number; a helpful lowercase name here
        would be a second vocabulary the public readers do not share."""
        emitted = re.findall(r'buf_append[sf]?\(out,\s*"((?:[^"\\]|\\.)*)"', SOURCE)
        body = " ".join(emitted)
        for name in ("done", "interrupted", "denied", "step_limit"):
            self.assertNotIn(
                f'\\"{name}\\"',
                body,
                f"a lowercase stop reason {name!r} reached the line",
            )
        self.assertIn('\\"stop_reason\\":%u', body)

    def test_error_codes_use_the_public_status_macros(self) -> None:
        mapping = re.findall(
            r"case (TNY_EVENT_ERROR_[A-Z]+): return (TNY_STATUS_[A-Z]+);", SOURCE
        )
        self.assertEqual(
            dict(mapping),
            {
                "TNY_EVENT_ERROR_IO": "TNY_STATUS_IO",
                "TNY_EVENT_ERROR_PROTOCOL": "TNY_STATUS_PROTOCOL",
                "TNY_EVENT_ERROR_BACKPRESSURE": "TNY_STATUS_BACKPRESSURE",
                "TNY_EVENT_ERROR_AUTH": "TNY_STATUS_AUTH",
                "TNY_EVENT_ERROR_OOM": "TNY_STATUS_OOM",
            },
        )
        for macro in set(dict(mapping).values()) | {
            "TNY_STATUS_OK",
            "TNY_STATUS_INTERNAL",
        }:
            self.assertRegex(HEADER, rf"#define\s+{macro}\b")


class BindingParityTest(unittest.TestCase):
    """The names we emit are the ones the shipped SDKs already use."""

    def test_generated_python_kinds(self) -> None:
        generated = (ROOT / "sdk/schema/generated/events.py").read_text()
        namespace: dict[str, object] = {}
        exec(compile(generated, "events.py", "exec"), namespace)  # noqa: S102
        kinds = namespace["EVENT_KINDS"]
        self.assertEqual(
            kinds, {event["type"]: event["id"] for event in SCHEMA["events"]}
        )
        self.assertEqual(
            namespace["EVENT_FIELDS"],
            {event["type"]: tuple(event["fields"]) for event in SCHEMA["events"]},
        )

    def test_typescript_addon_type_table(self) -> None:
        addon = (ROOT / "sdk/typescript/native/events.c").read_text()
        block = re.search(
            r"static const char \*event_type\(uint32_t kind\) \{(.+?)\}", addon, re.S
        )
        assert block is not None
        names = re.findall(r'"([a-z_]+)"', block.group(1))
        self.assertEqual(names, TYPES)

    def test_python_sdk_reads_numeric_stop_reasons(self) -> None:
        runtime = (ROOT / "sdk/python/src/tny/runtime.py").read_text()
        self.assertIn("stop_reason=int(view.stop_reason)", runtime)


if __name__ == "__main__":
    unittest.main()
