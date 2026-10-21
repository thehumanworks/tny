#!/usr/bin/env python3
"""Loopback mock startup must not depend on external hostname resolution."""

import http.client
import json
import selectors
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Only this disposable child patches DNS; the fixtures themselves never patch
# process-global resolver behavior. Run their real entrypoints and ready lines.
CHILD = """
import runpy, sys
from unittest.mock import patch
sys.path.insert(0, str(__import__('pathlib').Path(sys.argv[1]).parent))
sys.argv = sys.argv[1:]
with patch('socket.getfqdn', side_effect=AssertionError('unexpected reverse DNS')):
    runpy.run_path(sys.argv[0], run_name='__main__')
"""


class FixtureStartupTests(unittest.TestCase):
    def check_fixture(self, script, ready_prefix, endpoint, expected):
        with subprocess.Popen(
            [sys.executable, "-c", CHILD, str(ROOT / script), "0"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ) as child:
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(child.stdout, selectors.EVENT_READ)
                    self.assertTrue(selector.select(10), "fixture did not report ready")
                    ready = child.stdout.readline().decode().strip()
                self.assertTrue(ready.startswith(ready_prefix), ready)
                port = int(ready.removeprefix(ready_prefix))
                self.assertGreater(port, 0)
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                try:
                    connection.request("GET", endpoint)
                    response = connection.getresponse()
                    self.assertEqual(response.status, 200)
                    body = json.loads(response.read())
                    for key, value in expected.items():
                        self.assertEqual(body[key], value)
                finally:
                    connection.close()
            finally:
                if script.endswith("toolkit_provider.py"):
                    child.stdin.close()  # Exercise its normal EOF shutdown path.
                else:
                    child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                    self.fail("fixture did not stop")
            self.assertNotIn(b"unexpected reverse DNS", child.stderr.read())
            if script.endswith("toolkit_provider.py"):
                self.assertEqual(child.returncode, 0)

    def test_openai_ready_and_catalog_without_dns(self):
        self.check_fixture(
            "tests/integration/mock_openai.py",
            "ready on ",
            "/v1/models",
            {"data": [{"id": "mock-model-1"}, {"id": "mock-model-2"}]},
        )

    def test_toolkit_ready_assets_and_shutdown_without_dns(self):
        self.check_fixture(
            "tests/fixtures/toolkit_provider.py",
            "http://127.0.0.1:",
            "/assets",
            {"token": "toolkit-fixture-token", "account": "toolkit-fixture-account"},
        )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
