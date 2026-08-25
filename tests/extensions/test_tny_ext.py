import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from tny_ext import (  # noqa: E402
    BeforeAgentStartEvent,
    ContextAction,
    ContinueAction,
    ExtensionAPI,
    PermissionRequestEvent,
    TurnEndEvent,
    UnknownEvent,
    context,
    continue_with,
)
from tny_ext.actions import action_to_dict  # noqa: E402
from tny_ext.events import event_from_dict  # noqa: E402


class EventTests(unittest.TestCase):
    def test_known_event_is_typed_and_preserves_extra_payload(self):
        event = event_from_dict(
            {
                "schema_version": 1,
                "event_id": "event-7",
                "type": "before_agent_start",
                "sequence": 7,
                "provider": "openai",
                "session_id": "session-1",
                "turn_id": "turn-2",
                "timestamp_ms": 123,
                "payload": {
                    "prompt": "hello",
                    "system_prompt": "system",
                    "images": [{"type": "image", "data": "x"}],
                    "future_field": 42,
                },
            }
        )
        self.assertIsInstance(event, BeforeAgentStartEvent)
        self.assertEqual((event.event_id, event.sequence, event.provider), ("event-7", 7, "openai"))
        self.assertEqual(event.prompt, "hello")
        self.assertEqual(event.system_prompt, "system")
        self.assertEqual(event.images[0]["data"], "x")
        self.assertEqual(event.payload["future_field"], 42)

    def test_internal_permission_alias_uses_public_typed_event(self):
        event = event_from_dict(
            {"type": "permission", "perm_id": "p1", "perm_summary": "run", "perm_options": 7}
        )
        self.assertIsInstance(event, PermissionRequestEvent)
        self.assertEqual((event.permission_id, event.summary, event.options), ("p1", "run", 7))
        self.assertEqual(event.type, "permission")

    def test_stop_reason_accepts_string_or_structured_wire_value(self):
        simple = event_from_dict({"type": "turn_end", "stop": "completed"})
        structured = event_from_dict(
            {"type": "turn_end", "payload": {"stop": {"reason": "interrupted", "provider": "cancelled"}}}
        )
        self.assertIsInstance(simple, TurnEndEvent)
        self.assertEqual(simple.stop.reason, "completed")
        self.assertEqual(structured.stop.reason, "interrupted")
        self.assertEqual(structured.stop.details["provider"], "cancelled")

    def test_unknown_event_is_forward_compatible(self):
        event = event_from_dict({"type": "provider_added_later", "nested": {"ok": True}})
        self.assertIsInstance(event, UnknownEvent)
        self.assertEqual(event.payload, {"nested": {"ok": True}})


class ActionTests(unittest.TestCase):
    def test_visible_defaults_and_wire_shape(self):
        self.assertEqual(
            action_to_dict(context("policy", custom_type="guard")),
            {
                "kind": "context",
                "type": "context",
                "content": "policy",
                "custom_type": "guard",
                "display": True,
            },
        )
        self.assertEqual(
            action_to_dict(continue_with("again")),
            {
                "kind": "continue",
                "type": "continue",
                "content": "again",
                "message_kind": "user",
                "display": True,
            },
        )
        self.assertEqual(
            action_to_dict(continue_with("again", message_kind="custom")),
            {
                "kind": "continue",
                "type": "continue",
                "content": "again",
                "message_kind": "custom",
                "custom_type": "tny_extension",
                "display": True,
            },
        )

    def test_invalid_continuation_kind_is_rejected(self):
        with self.assertRaises(ValueError):
            ContinueAction("again", message_kind="assistant")


class RegistrationTests(unittest.TestCase):
    def test_decorator_and_direct_registration_keep_order(self):
        api = ExtensionAPI("test")

        @api.on("agent_end")
        def first(_event):
            return ContextAction("one")

        def second(_event):
            return None

        returned = api.on(BeforeAgentStartEvent, second)
        self.assertIs(returned, second)
        self.assertEqual([item.event for item in api.registrations], ["agent_end", "before_agent_start"])
        self.assertEqual([item.index for item in api.registrations], [0, 1])


if __name__ == "__main__":
    unittest.main()
