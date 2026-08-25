import json
import os
import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST = ROOT / "python" / "tny_extension_host.py"
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"


class HostProcess:
    def __init__(self, env=None):
        host_env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        if env:
            host_env.update(env)
        self.process = subprocess.Popen(
            [sys.executable, str(HOST)],
            cwd=str(ROOT),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=host_env,
        )

    def request(self, value):
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(json.dumps(value, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            stderr = self.process.stderr.read() if self.process.stderr is not None else ""
            raise AssertionError("host closed stdout unexpectedly: " + stderr)
        return json.loads(line)

    def raw_request(self, line):
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()
        return json.loads(self.process.stdout.readline())

    def close(self):
        if self.process.poll() is None:
            try:
                self.request({"id": "shutdown", "op": "shutdown"})
            except (BrokenPipeError, AssertionError):
                pass
        stdout, stderr = self.process.communicate(timeout=5)
        return stdout, stderr, self.process.returncode


class ExtensionHostTests(unittest.TestCase):
    def setUp(self):
        self.host = HostProcess()

    def tearDown(self):
        if self.host.process.poll() is None:
            self.host.close()

    def test_persistent_host_loads_files_and_package_and_invokes_one_handler(self):
        initialized = self.host.request(
            {
                "id": 1,
                "op": "initialize",
                "entries": [
                    str(FIXTURES / "lifecycle_extension.py"),
                    str(FIXTURES / "broken_setup.py"),
                    str(FIXTURES / "package_extension"),
                ],
            }
        )
        self.assertTrue(initialized["ok"])
        self.assertEqual(initialized["protocol"], 1)
        self.assertEqual(len(initialized["extensions"]), 2)
        self.assertEqual(len(initialized["load_errors"]), 1)
        self.assertNotIn("traceback", initialized["load_errors"][0])
        subscriptions = initialized["subscriptions"]
        self.assertEqual(
            [(item["event"], item["handler_id"]) for item in subscriptions],
            [
                ("before_agent_start", "0:before_agent_start:0"),
                ("agent_end", "0:agent_end:1"),
                ("agent_settled", "0:agent_settled:2"),
                ("text_delta", "0:text_delta:3"),
                ("status", "0:status:4"),
                ("future_event", "0:future_event:5"),
                ("session_start", "2:session_start:0"),
            ],
        )

        context_result = self.host.request(
            {
                "id": 2,
                "op": "invoke",
                "handler_id": "0:before_agent_start:0",
                "event": {"type": "before_agent_start", "prompt": "ship", "future": 1},
            }
        )
        self.assertEqual(
            context_result["action"],
            {
                "kind": "context",
                "type": "context",
                "content": "context for ship",
                "custom_type": "fixture_context",
                "display": True,
            },
        )

        continuation = self.host.request(
            {
                "id": 3,
                "op": "invoke",
                "handler_id": "0:agent_end:1",
                "event": {"type": "agent_end", "messages": []},
            }
        )
        self.assertEqual(
            continuation["action"],
            {
                "kind": "continue",
                "type": "continue",
                "content": "review the result",
                "message_kind": "user",
                "display": True,
            },
        )

        stopped = self.host.request(
            {
                "id": 4,
                "op": "invoke",
                "handler_id": "0:agent_settled:2",
                "event": {"type": "agent_settled"},
            }
        )
        self.assertEqual(
            stopped["action"],
            {"kind": "stop", "type": "stop", "reason": "fixture condition reached"},
        )

        none_result = self.host.request(
            {
                "id": 5,
                "op": "invoke",
                "handler_id": "0:text_delta:3",
                "event": {"type": "text_delta", "text": "chunk"},
            }
        )
        self.assertEqual(none_result["action"], {"kind": "none", "type": "none"})

        package_result = self.host.request(
            {
                "id": 6,
                "op": "invoke",
                "handler_id": "2:session_start:0",
                "event": {"type": "session_start", "reason": "startup"},
            }
        )
        self.assertEqual(package_result["action"]["content"], "loaded through relative import")

        stdout, stderr, returncode = self.host.close()
        self.assertEqual(returncode, 0)
        self.assertEqual(stdout, "")
        self.assertIn("fixture setup noise", stderr)
        self.assertIn("broken setup noise", stderr)

    def test_explicit_index_path_is_also_loaded_as_a_package(self):
        initialized = self.host.request(
            {
                "id": 1,
                "op": "initialize",
                "entries": [str(FIXTURES / "package_extension" / "index.py")],
            }
        )
        self.assertTrue(initialized["ok"])
        self.assertEqual(initialized["extensions"][0]["name"], "package_extension")
        self.assertEqual(initialized["subscriptions"][0]["extension"], "package_extension")
        result = self.host.request(
            {
                "id": 2,
                "op": "invoke",
                "handler_id": "0:session_start:0",
                "event": {"type": "session_start", "reason": "startup"},
            }
        )
        self.assertEqual(result["action"]["content"], "loaded through relative import")

    def test_handler_failure_is_structured_and_host_remains_alive(self):
        initialized = self.host.request(
            {"id": 1, "op": "initialize", "entries": [str(FIXTURES / "lifecycle_extension.py")]}
        )
        self.assertTrue(initialized["ok"])
        failed = self.host.request(
            {
                "id": 2,
                "op": "invoke",
                "handler_id": "0:status:4",
                "event": {"type": "status", "text": "working"},
            }
        )
        self.assertFalse(failed["ok"])
        self.assertEqual(failed["error"]["kind"], "extension_error")
        self.assertEqual(failed["error"]["message"], "fixture handler failed")
        self.assertNotIn("traceback", failed["error"])

        future = self.host.request(
            {
                "id": 3,
                "op": "invoke",
                "handler_id": "0:future_event:5",
                "event": {"type": "future_event", "instruction": "continue safely", "new_field": True},
            }
        )
        self.assertTrue(future["ok"])
        self.assertEqual(
            future["action"],
            {
                "kind": "continue",
                "type": "continue",
                "content": "continue safely",
                "message_kind": "custom",
                "custom_type": "future_fixture",
                "display": True,
            },
        )

    def test_wildcard_handler_accepts_each_event_type(self):
        initialized = self.host.request(
            {"id": 1, "op": "initialize",
             "entries": [str(FIXTURES / "wildcard_extension.py")]}
        )
        self.assertTrue(initialized["ok"])
        handler_id = initialized["subscriptions"][0]["handler_id"]
        for request_id, event_type in enumerate(("session_start", "text_delta"), 2):
            result = self.host.request(
                {"id": request_id, "op": "invoke", "handler_id": handler_id,
                 "event": {"type": event_type}}
            )
            self.assertTrue(result["ok"])
            self.assertEqual(result["action"]["content"], event_type)

    def test_debug_traceback_is_bounded_and_protocol_errors_recover(self):
        initialized = self.host.request(
            {
                "id": 1,
                "op": "initialize",
                "debug": True,
                "entries": [str(FIXTURES / "lifecycle_extension.py")],
            }
        )
        self.assertTrue(initialized["ok"])
        failed = self.host.request(
            {
                "id": 2,
                "op": "invoke",
                "handler_id": "0:status:4",
                "event": {"type": "status", "text": "working"},
            }
        )
        self.assertIn("RuntimeError: fixture handler failed", failed["error"]["traceback"])
        self.assertLessEqual(len(failed["error"]["traceback"]), 4096)

        malformed = self.host.raw_request("not-json")
        self.assertFalse(malformed["ok"])
        self.assertEqual(malformed["error"]["kind"], "protocol_error")
        ping = self.host.request({"id": 4, "op": "ping"})
        self.assertEqual(ping, {"id": 4, "ok": True, "protocol": 1})


if __name__ == "__main__":
    unittest.main()
