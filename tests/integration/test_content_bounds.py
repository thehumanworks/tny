#!/usr/bin/env python3
"""Large fetched content stays locally recoverable without another HTTP request."""

import hashlib
import json
import re
import stat
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import test_execution_code_mode as fixture


class Pages(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        body, kind = self.server.pages[self.path.split("?", 1)[0]]
        self.send_response(200)
        self.send_header("Content-Type", kind)
        length = self.server.declared_lengths.get(self.path, len(body))
        self.send_header("Content-Length", str(length))
        self.end_headers()
        try:
            self.wfile.write(body)
            if length != len(body):
                self.close_connection = True
        except (BrokenPipeError, ConnectionResetError):
            pass


@unittest.skipIf(fixture.WASM, "native execution; wasm has a clean unsupported seam")
class ContentBounds(unittest.TestCase):
    close_server = fixture.ExecutionCodeMode.close_server
    start = fixture.ExecutionCodeMode.start
    run_code = fixture.ExecutionCodeMode.run_code
    finish_sequence = fixture.ExecutionCodeMode.finish_sequence

    def setUp(self):
        fixture.ExecutionCodeMode.setUp(self)
        self.web = ThreadingHTTPServer(("127.0.0.1", 0), Pages)
        self.web.pages = {}
        self.web.requests = []
        self.web.declared_lengths = {}
        self.web_thread = threading.Thread(target=self.web.serve_forever, daemon=True)
        self.web_thread.start()
        self.addCleanup(self.close_web)
        self.base = f"http://127.0.0.1:{self.web.server_port}"
        self.settings = self.home / ".tny" / "settings.json"
        self.settings.parent.mkdir(exist_ok=True)
        self.settings.write_text(json.dumps({"max_tool_result_bytes": 1024}))

    def close_web(self):
        self.web.shutdown()
        self.web_thread.join(timeout=3)
        self.web.server_close()

    def test_saved_markdown_html_and_search_override_recover_without_refetch(self):
        for wire in ("responses", "chat"):
            for kind in ("text/markdown", "text/html"):
                with self.subTest(wire=wire, content_type=kind):
                    body = (b"# preserved <tag> &amp; body\n" * 5000) + b"LAST-LINE\n"
                    self.web.pages["/page"] = (body, kind)
                    self.web.requests.clear()
                    url = self.base + "/page?secret=not-an-artifact-name"
                    code = (
                        "import json, re, hashlib, os\n"
                        f"notice = tools.call('web_fetch', {json.dumps({'url': url})!r})\n"
                        f"assert {kind!r} in notice, notice\n"
                        "assert 'original response bytes, no conversion' in notice\n"
                        "assert 'LAST-LINE' not in notice\n"
                        "path = re.search(r'Local session file: (.+)', notice).group(1)\n"
                        "assert 'secret' not in path\n"
                        "assert os.stat(path).st_mode & 0o077 == 0\n"
                        "assert os.stat(os.path.dirname(path)).st_mode & 0o077 == 0\n"
                        "handle = re.search(r'handle=\"([0-9a-f]+)\"', notice).group(1)\n"
                        "parts = []\n"
                        f"for offset in range(0, {len(body)}, 4096):\n"
                        "    parts.append(tools.call('read_tool_result', json.dumps(dict(handle=handle, offset=offset, length=4096))))\n"
                        "recovered = ''.join(parts).encode()\n"
                        f"assert hashlib.sha256(recovered).hexdigest() == {hashlib.sha256(body).hexdigest()!r}\n"
                        "assert open(path, 'rb').read() == recovered\n"
                        "print(path)\n"
                    )
                    output = self.run_code(code, wire=wire, no_save=False)
                    self.assertNotIn("Traceback", output)
                    paths = list(
                        (self.home / ".tny").glob("sessions/*/*/results/*.txt")
                    )
                    self.assertTrue(any(path.read_bytes() == body for path in paths))
                    self.assertEqual(
                        self.web.requests, ["/page?secret=not-an-artifact-name"]
                    )
                    self.assertTrue(
                        all(
                            stat.S_IMODE(path.stat().st_mode) == 0o600 for path in paths
                        )
                    )
                    self.assertFalse(list((self.home / ".tny").rglob("*.tmp.*")))

            self.settings.write_text(
                json.dumps(
                    {
                        "max_tool_result_bytes": 1024,
                        "web_search_url": self.base + "/page?q={query}",
                    }
                )
            )
            self.web.requests.clear()
            output = self.run_code(
                "print(tools.call('web_search', '{\"query\":\"find page\"}'))",
                wire=wire,
                no_save=False,
            )
            self.assertIn("Local session file:", output)
            self.assertNotIn("LAST-LINE", output)
            self.assertEqual(self.web.requests, ["/page?q=find%20page"])

    def test_saved_two_megabyte_page_retains_bytes_beyond_old_download_cap(self):
        body = b"a" * (2 * 1024 * 1024) + b"BEYOND-OLD-CAP"
        self.web.pages["/two-megabytes"] = (body, "text/markdown")
        for wire in ("responses", "chat"):
            self.web.requests.clear()
            arguments = json.dumps({"url": self.base + "/two-megabytes"})
            code = (
                "import json, re, hashlib\n"
                f"notice = tools.call('web_fetch', {arguments!r})\n"
                "assert 'INCOMPLETE' not in notice, notice\n"
                "path = re.search(r'Local session file: (.+)', notice).group(1)\n"
                f"assert hashlib.sha256(open(path, 'rb').read()).hexdigest() == {hashlib.sha256(body).hexdigest()!r}\n"
                "handle = re.search(r'handle=\"([0-9a-f]+)\"', notice).group(1)\n"
                "assert tools.call('read_tool_result', json.dumps(dict(handle=handle, offset=2097152, length=14))) == 'BEYOND-OLD-CAP'\n"
                "print('two megabytes retained exactly')"
            )
            output = self.run_code(code, wire=wire, no_save=False)
            self.assertIn("two megabytes retained exactly", output)
            self.assertEqual(self.web.requests, ["/two-megabytes"])

    def test_interrupted_body_retains_only_received_prefix(self):
        self.web.pages["/interrupted"] = (b"p" * 4096, "text/markdown")
        self.web.declared_lengths["/interrupted"] = 8192
        arguments = json.dumps({"url": self.base + "/interrupted"})
        code = (
            "import json, re\n"
            f"notice = tools.call('web_fetch', {arguments!r})\n"
            "assert 'INCOMPLETE: response interrupted' in notice, notice\n"
            "assert '4096 bytes retained' in notice, notice\n"
            "path = re.search(r'Local session file: (.+)', notice).group(1)\n"
            "assert open(path, 'rb').read() == b'p' * 4096\n"
            "handle = re.search(r'handle=\"([0-9a-f]+)\"', notice).group(1)\n"
            "assert tools.call('read_tool_result', json.dumps(dict(handle=handle, offset=0, length=8192))) == 'p' * 4096\n"
            "print('interrupted prefix remains readable')"
        )
        output = self.run_code(code, no_save=False)
        self.assertIn("interrupted prefix remains readable", output)
        self.assertEqual(self.web.requests, ["/interrupted"])

    def test_storage_success_is_not_inferred_from_path_text(self):
        home = self.home / "recovery unavailable"
        settings = home / ".tny" / "settings.json"
        settings.parent.mkdir(parents=True)
        settings.write_text(self.settings.read_text())
        self.web.pages["/path"] = (b"q" * 2048, "text/markdown")
        arguments = json.dumps({"url": self.base + "/path"})
        code = (
            "import re\n"
            f"notice = tools.call('web_fetch', {arguments!r})\n"
            "assert notice.startswith('HTTP 200'), notice\n"
            "path = re.search(r'Local session file: (.+)', notice).group(1)\n"
            "assert 'recovery unavailable' in path\n"
            "assert open(path, 'rb').read() == b'q' * 2048\n"
            "print('storage status is independent of path text')"
        )
        output = self.run_code(code, no_save=False, env={"HOME": str(home)})
        self.assertIn("storage status is independent of path text", output)
        self.assertEqual(self.web.requests, ["/path"])

    def test_content_handles_survive_fresh_execution_cells(self):
        self.web.pages["/retained"] = (b"x" * 5000 + b"RECOVER-ME", "text/markdown")
        for wire in ("responses", "chat"):
            for saved in (False, True):
                with self.subTest(wire=wire, saved=saved):
                    self.web.requests.clear()

                    def recover(outputs):
                        handle = re.search(r'handle="([0-9a-f]+)"', outputs[0]).group(1)
                        arguments = json.dumps(
                            dict(handle=handle, offset=5000, length=10)
                        )
                        return {
                            "code": f"print(tools.call('read_tool_result', {arguments!r}))"
                        }

                    arguments = json.dumps({"url": self.base + "/retained"})
                    self.server.sequence = [
                        {"code": f"print(tools.call('web_fetch', {arguments!r}))"},
                        recover,
                    ]
                    self.finish_sequence(wire=wire, no_save=not saved)
                    self.assertIn("RECOVER-ME", self.server.outputs[1])
                    self.assertEqual(self.web.requests, ["/retained"])
                    self.server.sequence = None

    def test_storage_failure_has_no_false_path_handle_or_partial_file(self):
        self.web.pages["/blocked"] = (b"unretained-content\n" * 5000, "text/markdown")
        for wire in ("responses", "chat"):
            self.web.requests.clear()
            arguments = json.dumps({"url": self.base + "/blocked"})
            code = (
                "import glob, os\n"
                "sessions = [p for p in glob.glob(os.path.join(os.environ['HOME'], '.tny', 'sessions', '*', '*')) if os.path.isdir(p)]\n"
                "assert sessions, 'saved session missing'\n"
                "for session in sessions:\n"
                "    blocker = os.path.join(session, 'results')\n"
                "    if not os.path.exists(blocker):\n"
                "        with open(blocker, 'w') as stream: stream.write('blocked')\n"
                f"notice = tools.call('web_fetch', {arguments!r})\n"
                "assert notice.startswith('error: could not retain fetched content:'), notice\n"
                "assert 'content recovery unavailable' in notice, notice\n"
                "assert 'Local session file:' not in notice, notice\n"
                "assert 'handle=' not in notice, notice\n"
                "assert 'unretained-content' not in notice, notice\n"
                "print('storage failure reported honestly')"
            )
            output = self.run_code(code, wire=wire, no_save=False)
            self.assertIn("storage failure reported honestly", output)
            self.assertEqual(self.web.requests, ["/blocked"])
            self.assertFalse(list((self.home / ".tny").rglob("*.tmp.*")))
            blockers = list((self.home / ".tny").glob("sessions/*/*/results"))
            self.assertTrue(blockers)
            self.assertTrue(
                all(
                    path.is_file() and path.read_text() == "blocked"
                    for path in blockers
                )
            )

    def test_small_and_exact_threshold_content_stays_unchanged(self):
        for size in (0, 7, 1024, 1025):
            self.web.pages[f"/{size}"] = (b"s" * size, "text/markdown")
        for wire in ("responses", "chat"):
            code = "import json\n"
            for size in (0, 7, 1024):
                url = self.base + f"/{size}"
                expected = f"HTTP 200 from {url}\n\n" + "s" * size
                code += f"assert tools.call('web_fetch', {json.dumps({'url': url})!r}) == {expected!r}\n"
            code += f"print(tools.call('web_fetch', {json.dumps({'url': self.base + '/1025'})!r}))"
            output = self.run_code(code, wire=wire, no_save=False)
            self.assertNotIn("Traceback", output)
            self.assertIn("Content saved (1025 bytes)", output)
            self.assertNotIn("s" * 100, output)

    def test_ephemeral_parallel_fetch_budget_keeps_first_content_without_disk(self):
        self.web.pages["/large"] = (b"a" * 600000, "text/markdown")
        calls = [{"name": "web_fetch", "arguments": {"url": self.base + "/large"}}] * 2
        for wire in ("responses", "chat"):
            self.web.requests.clear()
            code = (
                "import json, re\n"
                f"results = json.loads(tools.parallel({json.dumps(calls)!r}))\n"
                "assert 'Ephemeral: memory only, no file' in results[0], results\n"
                "assert 'content recovery unavailable' in results[1], results\n"
                "assert 'no content returned' in results[1]\n"
                "assert all('Local session file:' not in r for r in results)\n"
                "handle = re.search(r'handle=\"([0-9a-f]+)\"', results[0]).group(1)\n"
                "assert tools.call('read_tool_result', json.dumps(dict(handle=handle, offset=599990, length=10))) == 'a' * 10\n"
                "print('retained first content; second refused cleanly')"
            )
            output = self.run_code(code, wire=wire)
            self.assertIn("retained first content; second refused cleanly", output)
            self.assertEqual(self.web.requests, ["/large", "/large"])
            self.assertFalse(list((self.home / ".tny").glob("sessions/*/*/results/*")))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
