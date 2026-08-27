import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from tny_ext import (  # noqa: E402
    BeforeAgentStartEvent,
    CapabilityView,
    ContextAction,
    ContinueAction,
    ExtensionAPI,
    PermissionRequestEvent,
    TurnEndEvent,
    UnknownEvent,
    annotate_tool,
    block_prompt,
    context,
    continue_with,
    decide_permission,
    deny_tool,
    replace_tool_result,
    rewrite_tool,
    transform_prompt,
)
from tny_ext.actions import action_to_dict  # noqa: E402
from tny_ext.capabilities import capability_view_from_dict  # noqa: E402
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
        self.assertEqual(
            (event.event_id, event.sequence, event.provider), ("event-7", 7, "openai")
        )
        self.assertEqual(event.prompt, "hello")
        self.assertEqual(event.system_prompt, "system")
        self.assertEqual(event.images[0]["data"], "x")
        self.assertEqual(event.payload["future_field"], 42)

    def test_internal_permission_alias_uses_public_typed_event(self):
        event = event_from_dict(
            {
                "type": "permission",
                "perm_id": "p1",
                "perm_summary": "run",
                "perm_options": 7,
            }
        )
        self.assertIsInstance(event, PermissionRequestEvent)
        self.assertEqual(
            (event.permission_id, event.summary, event.options), ("p1", "run", 7)
        )
        self.assertEqual(event.type, "permission")

    def test_stop_reason_accepts_string_or_structured_wire_value(self):
        simple = event_from_dict({"type": "turn_end", "stop": "completed"})
        structured = event_from_dict(
            {
                "type": "turn_end",
                "payload": {"stop": {"reason": "interrupted", "provider": "cancelled"}},
            }
        )
        self.assertIsInstance(simple, TurnEndEvent)
        self.assertEqual(simple.stop.reason, "completed")
        self.assertEqual(structured.stop.reason, "interrupted")
        self.assertEqual(structured.stop.details["provider"], "cancelled")

    def test_unknown_event_is_forward_compatible(self):
        event = event_from_dict(
            {"type": "provider_added_later", "nested": {"ok": True}}
        )
        self.assertIsInstance(event, UnknownEvent)
        self.assertEqual(event.payload, {"nested": {"ok": True}})

    def test_contracted_lifecycle_and_control_events_are_typed(self):
        cases = {
            "user_prompt_submit": (
                "UserPromptSubmitEvent",
                {
                    "prompt": "ship",
                    "source": "user",
                    "submission_id": "p1",
                    "images": [],
                },
                ("source", "user"),
            ),
            "turn_start": (
                "TurnStartEvent",
                {"iteration": 2, "source": "continuation"},
                ("iteration", 2),
            ),
            "message_start": (
                "MessageStartEvent",
                {"message_id": "m1", "role": "assistant", "content_type": "text"},
                ("message_id", "m1"),
            ),
            "message_update": (
                "MessageUpdateEvent",
                {
                    "message_id": "m1",
                    "role": "assistant",
                    "content_type": "text",
                    "text": "a",
                },
                ("text", "a"),
            ),
            "message_end": (
                "MessageEndEvent",
                {
                    "message_id": "m1",
                    "role": "assistant",
                    "content_type": "text",
                    "text": "all",
                },
                ("text", "all"),
            ),
            "pre_compact": (
                "PreCompactEvent",
                {"trigger": "manual", "message_count": 8},
                ("trigger", "manual"),
            ),
            "post_compact": (
                "PostCompactEvent",
                {
                    "trigger": "manual",
                    "before_count": 8,
                    "after_count": 2,
                    "summary": "bounded",
                },
                ("after_count", 2),
            ),
            "compact_failed": (
                "CompactFailedEvent",
                {"trigger": "threshold", "error": "oom"},
                ("error", "oom"),
            ),
            "model_change": (
                "ModelChangeEvent",
                {"previous": "a", "current": "b", "source": "command"},
                ("current", "b"),
            ),
            "effort_change": (
                "EffortChangeEvent",
                {"previous": "low", "current": "high", "source": "command"},
                ("current", "high"),
            ),
            "instructions_change": (
                "InstructionsChangeEvent",
                {"paths": ["AGENTS.md"], "digest": "abc", "count": 1},
                ("paths", ("AGENTS.md",)),
            ),
            "workspace_change": (
                "WorkspaceChangeEvent",
                {"action": "add", "path": "/tmp/ws", "directories": ["/tmp/ws"]},
                ("directories", ("/tmp/ws",)),
            ),
            "subagent_start": (
                "SubagentStartEvent",
                {"subagent_id": "s1", "action": "create"},
                ("subagent_id", "s1"),
            ),
            "subagent_end": (
                "SubagentEndEvent",
                {
                    "subagent_id": "s1",
                    "action": "create",
                    "outcome": "done",
                    "ok": True,
                },
                ("ok", True),
            ),
            "pre_tool_use": (
                "PreToolUseEvent",
                {
                    "tool_name": "read_file",
                    "tool_id": "t1",
                    "arguments": {"path": "a"},
                    "original_arguments": {"path": "b"},
                },
                ("arguments", {"path": "a"}),
            ),
            "post_tool_use": (
                "PostToolUseEvent",
                {
                    "tool_name": "read_file",
                    "tool_id": "t1",
                    "result": "ok",
                    "original_ok": True,
                },
                ("original_ok", True),
            ),
            "post_tool_failure": (
                "PostToolFailureEvent",
                {
                    "tool_name": "read_file",
                    "tool_id": "t1",
                    "result": "error",
                    "original_ok": False,
                },
                ("original_ok", False),
            ),
            "post_tool_batch": (
                "PostToolBatchEvent",
                {"tool_ids": ["t1", "t2"], "failed": 1},
                ("tool_ids", ("t1", "t2")),
            ),
            "provider_request": (
                "ProviderRequestEvent",
                {
                    "method": "POST",
                    "endpoint": "/v1/responses",
                    "metadata": {"wire": "responses"},
                },
                ("endpoint", "/v1/responses"),
            ),
            "provider_response": (
                "ProviderResponseEvent",
                {"status": 200, "metadata": {"stream": True}},
                ("status", 200),
            ),
        }
        for event_type, (class_name, payload, field) in cases.items():
            with self.subTest(event_type=event_type):
                event = event_from_dict({"type": event_type, "payload": payload})
                self.assertEqual(type(event).__name__, class_name)
                self.assertEqual(getattr(event, field[0]), field[1])


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

    def test_contracted_control_actions_have_frozen_wire_names(self):
        self.assertEqual(
            action_to_dict(transform_prompt("safer prompt")),
            {
                "kind": "prompt_transform",
                "type": "prompt_transform",
                "prompt": "safer prompt",
            },
        )
        self.assertEqual(action_to_dict(block_prompt("policy"))["kind"], "prompt_block")
        self.assertEqual(
            action_to_dict(rewrite_tool({"path": "README.md"}))["arguments"],
            {"path": "README.md"},
        )
        self.assertEqual(
            action_to_dict(deny_tool("outside workspace"))["kind"], "tool_deny"
        )
        self.assertEqual(
            action_to_dict(decide_permission("allow_once"))["decision"], "allow_once"
        )
        self.assertEqual(
            action_to_dict(annotate_tool("checked"))["kind"], "tool_annotate"
        )
        self.assertEqual(
            action_to_dict(replace_tool_result("redacted", is_error=True))["is_error"],
            True,
        )
        with self.assertRaises(ValueError):
            decide_permission("allow_always")


class CapabilityTests(unittest.TestCase):
    def test_view_is_immutable_and_preserves_unknown_entries_and_states(self):
        view = capability_view_from_dict(
            {
                "schema_version": 1,
                "selected_provider": "openai",
                "extension_runtime": {"enabled": True, "python": "available"},
                "providers": {
                    "openai": {
                        "provider": "openai",
                        "runtime": "native",
                        "entries": {
                            "extensions.prompt.observe": {
                                "state": "supported",
                                "reason": "implemented",
                            },
                            "extensions.future.mode": {
                                "state": "experimental_later",
                                "reason": "future",
                                "nested": {"values": [1, 2]},
                            },
                        },
                    }
                },
                "future_top_level": {"enabled": True},
            }
        )
        self.assertIsInstance(view, CapabilityView)
        self.assertTrue(view.extension_enabled)
        self.assertEqual(view.selected_provider, "openai")
        self.assertIsNotNone(view.selected)
        self.assertTrue(view.selected.supports("extensions.prompt.observe"))
        future = view.selected.get("extensions.future.mode")
        self.assertIsNotNone(future)
        self.assertEqual(future.state, "experimental_later")
        self.assertFalse(future.known_state)
        self.assertEqual(future.extra["nested"]["values"], (1, 2))
        with self.assertRaises(TypeError):
            view.extra["future_top_level"] = False
        with self.assertRaises(TypeError):
            future.extra["nested"]["new"] = True

    def test_extension_api_keeps_legacy_constructor_and_explicit_view(self):
        legacy = ExtensionAPI("legacy")
        self.assertEqual(legacy.capabilities.selected_provider, "unknown")
        view = capability_view_from_dict(
            {"schema_version": 1, "selected_provider": "codex", "providers": {}}
        )
        self.assertIs(ExtensionAPI("modern", view).capabilities, view)


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
        self.assertEqual(
            [item.event for item in api.registrations],
            ["agent_end", "before_agent_start"],
        )
        self.assertEqual([item.index for item in api.registrations], [0, 1])


if __name__ == "__main__":
    unittest.main()
