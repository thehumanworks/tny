import json
import pathlib
import tempfile
import unittest

from test_extension_host import HostProcess


ROOT = pathlib.Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "examples" / "extensions"


class ExtensionExamplesTests(unittest.TestCase):
    def test_every_example_loads_and_performs_its_documented_action(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            context_file = root / "context.md"
            context_file.write_text("Use the release checklist.\n", encoding="utf-8")
            event_log = root / "events.jsonl"
            host = HostProcess(
                {
                    "TNY_CONTEXT_FILE": str(context_file),
                    "TNY_EVENT_LOG": str(event_log),
                }
            )
            try:
                initialized = host.request(
                    {
                        "id": 1,
                        "op": "initialize",
                        "entries": [
                            str(EXAMPLES / "ci_guard"),
                            str(EXAMPLES / "log_events.py"),
                            str(EXAMPLES / "project_context.py"),
                            str(EXAMPLES / "stop_on_tool_failure.py"),
                            str(EXAMPLES / "verify_once.py"),
                        ],
                    }
                )
                self.assertTrue(initialized["ok"])
                self.assertEqual(initialized["load_errors"], [])
                self.assertEqual(len(initialized["extensions"]), 5)

                subscriptions = {
                    (item["extension"], item["event"]): item["handler_id"]
                    for item in initialized["subscriptions"]
                }

                logged = host.request(
                    {
                        "id": 2,
                        "op": "invoke",
                        "handler_id": subscriptions[("log_events", "*")],
                        "event": {
                            "type": "status",
                            "event_id": "event-2",
                            "sequence": 2,
                            "provider": "openai",
                            "session_id": "session-1",
                            "turn_id": "turn-1",
                            "timestamp_ms": 20,
                            "payload": {"text": "working"},
                        },
                    }
                )
                self.assertEqual(logged["action"]["kind"], "none")
                record = json.loads(event_log.read_text(encoding="utf-8"))
                self.assertEqual(record["type"], "status")
                self.assertEqual(record["provider"], "openai")

                context_result = host.request(
                    {
                        "id": 3,
                        "op": "invoke",
                        "handler_id": subscriptions[("project_context", "before_agent_start")],
                        "event": {
                            "type": "before_agent_start",
                            "session_id": "session-1",
                            "prompt": "ship",
                        },
                    }
                )
                self.assertEqual(context_result["action"]["kind"], "none")

                session_started = host.request(
                    {
                        "id": "context-session-start",
                        "op": "invoke",
                        "handler_id": subscriptions[("project_context", "session_start")],
                        "event": {"type": "session_start", "session_id": "session-1"},
                    }
                )
                self.assertEqual(session_started["action"]["kind"], "none")

                context_result = host.request(
                    {
                        "id": "context-first-turn",
                        "op": "invoke",
                        "handler_id": subscriptions[("project_context", "before_agent_start")],
                        "event": {
                            "type": "before_agent_start",
                            "session_id": "session-1",
                            "prompt": "ship",
                        },
                    }
                )
                self.assertEqual(context_result["action"]["kind"], "context")
                self.assertEqual(context_result["action"]["custom_type"], "project_context")
                self.assertIn("release checklist", context_result["action"]["content"])

                context_again = host.request(
                    {
                        "id": "context-later-turn",
                        "op": "invoke",
                        "handler_id": subscriptions[("project_context", "before_agent_start")],
                        "event": {
                            "type": "before_agent_start",
                            "session_id": "session-1",
                            "prompt": "ship again",
                        },
                    }
                )
                self.assertEqual(context_again["action"]["kind"], "none")

                host.request(
                    {
                        "id": "context-next-session-start",
                        "op": "invoke",
                        "handler_id": subscriptions[("project_context", "session_start")],
                        "event": {"type": "session_start", "session_id": "session-2"},
                    }
                )
                context_next_session = host.request(
                    {
                        "id": "context-next-session-first-turn",
                        "op": "invoke",
                        "handler_id": subscriptions[("project_context", "before_agent_start")],
                        "event": {
                            "type": "before_agent_start",
                            "session_id": "session-2",
                            "prompt": "ship another session",
                        },
                    }
                )
                self.assertEqual(context_next_session["action"]["kind"], "context")

                stop_result = host.request(
                    {
                        "id": 4,
                        "op": "invoke",
                        "handler_id": subscriptions[("stop_on_tool_failure", "tool_end")],
                        "event": {
                            "type": "tool_end",
                            "tool_name": "tests",
                            "tool_id": "tool-1",
                            "detail": "exit 1",
                            "ok": False,
                        },
                    }
                )
                self.assertEqual(stop_result["action"]["kind"], "stop")
                self.assertIn("tests failed", stop_result["action"]["reason"])

                verify_result = host.request(
                    {
                        "id": 5,
                        "op": "invoke",
                        "handler_id": subscriptions[("verify_once", "agent_end")],
                        "event": {
                            "type": "agent_end",
                            "stop": {"reason": "done"},
                            "continuation_count": 0,
                            "max_continuations": 0,
                            "messages": [],
                            "output_text": "Implementation finished.",
                        },
                    }
                )
                self.assertEqual(verify_result["action"]["kind"], "continue")
                self.assertEqual(verify_result["action"]["message_kind"], "user")

                ci_tool = host.request(
                    {
                        "id": 6,
                        "op": "invoke",
                        "handler_id": subscriptions[("ci_guard", "tool_end")],
                        "event": {
                            "type": "tool_end",
                            "tool_name": "pytest",
                            "tool_id": "tool-2",
                            "detail": "failed",
                            "ok": False,
                        },
                    }
                )
                self.assertEqual(ci_tool["action"]["kind"], "none")
                ci_end = host.request(
                    {
                        "id": 7,
                        "op": "invoke",
                        "handler_id": subscriptions[("ci_guard", "agent_end")],
                        "event": {
                            "type": "agent_end",
                            "stop": {"reason": "done"},
                            "continuation_count": 0,
                            "max_continuations": 0,
                            "messages": [],
                            "output_text": "Tests failed.",
                        },
                    }
                )
                self.assertEqual(ci_end["action"]["kind"], "continue")
                self.assertEqual(ci_end["action"]["message_kind"], "custom")
                self.assertEqual(ci_end["action"]["custom_type"], "ci_guard")
                self.assertIn("pytest", ci_end["action"]["content"])
            finally:
                host.close()


if __name__ == "__main__":
    unittest.main()
