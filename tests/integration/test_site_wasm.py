#!/usr/bin/env python3
"""Browser smoke for the wasm landing terminal (docs/adr/0017).

Serves site/ plus the built web artifact, opens the page in headless
Chromium, feeds a key through the URL hash, and asserts:
  * the real TUI banner paints inside xterm.js,
  * one full turn streams from mock_openai.py (tool call included),
  * /quit exits the binary cleanly.

Requires playwright (python) and a chromium; skips with exit 0 when either
is unavailable so the fixture-only suite stays runnable everywhere. CI's
wasm job installs playwright and runs this for real.
"""
import http.server
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SITE = os.path.join(ROOT, "site")
WEB_MJS = os.path.join(ROOT, "build", "wasm", "tny-web.mjs")
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("test_site_wasm: skip (playwright not installed)")
    sys.exit(0)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    if not os.path.exists(WEB_MJS):
        print("test_site_wasm: skip (run `make wasm-web` first)")
        return

    # stage: site/ + assets/wasm/ artifact in a temp docroot
    with tempfile.TemporaryDirectory() as root:
        doc = os.path.join(root, "doc")
        shutil.copytree(SITE, doc)
        wasm_dir = os.path.join(doc, "assets", "wasm")
        os.makedirs(wasm_dir, exist_ok=True)
        shutil.copy(WEB_MJS, wasm_dir)
        shutil.copy(WEB_MJS.replace(".mjs", ".wasm"), wasm_dir)

        sport = free_port()

        class Quiet(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **kw):
                super().__init__(*a, directory=doc, **kw)

            def log_message(self, *a):
                pass

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", sport), Quiet)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()

        mport = free_port()
        mock = subprocess.Popen(
            [sys.executable, MOCK, str(mport)],
            env=dict(os.environ, MOCK_EXPECT_WIRE="responses", MOCK_CLEAN_EOF="1"),
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            line = mock.stdout.readline().decode()
            assert "ready" in line, f"mock did not start: {line!r}"

            url = (f"http://127.0.0.1:{sport}/index.html"
                   f"#OPENAI_API_KEY=test-key-not-real"
                   f"&OPENAI_BASE_URL=http://127.0.0.1:{mport}/v1")

            with sync_playwright() as pw:
                exe = os.environ.get("TNY_CHROMIUM", "/opt/pw-browsers/chromium")
                browser = (pw.chromium.launch(executable_path=exe)
                           if os.path.exists(exe) else pw.chromium.launch())
                page = browser.new_page()
                errors = []
                page.on("pageerror", lambda e: errors.append(str(e)))
                page.goto(url)
                print("stage: loaded", flush=True)

                def term_text():
                    return page.evaluate(
                        """() => {
                          const rows = document.querySelectorAll(
                            '[data-term-xterm] .xterm-rows > div');
                          return Array.from(rows).map(r => r.textContent).join('\\n');
                        }""")

                # banner: the real binary paints its version banner
                page.wait_for_function(
                    """() => document.querySelector('[data-term-xterm]')
                          && document.querySelector('[data-term-xterm]')
                             .textContent.includes('/help for commands')""",
                    timeout=30000)
                banner = term_text()
                assert "tny" in banner, banner
                # Native terminals apply ONLCR: every LF also returns to
                # column zero. The browser stdout sink emits raw LF, so the
                # xterm configuration must emulate that behavior. Otherwise
                # successive TUI rows staircase across and overflow the pane.
                help_rows = [line for line in banner.splitlines()
                             if "/help for commands" in line]
                assert help_rows, banner
                assert help_rows[0].startswith("/help for commands"), banner
                print("stage: banner", flush=True)

                # one full turn against the strict mock
                page.click("[data-term-xterm]")
                page.keyboard.type("list files in .")
                page.keyboard.press("Enter")
                page.wait_for_function(
                    """() => document.querySelector('[data-term-xterm]')
                             .textContent.includes('MOCK-OK')""",
                    timeout=30000)

                print("stage: turn", flush=True)
                # /quit exits cleanly (small settle: the TUI repaints after
                # the turn; typed keys must land in the composer, not race it)
                page.wait_for_timeout(800)
                page.keyboard.type("/quit", delay=40)
                page.keyboard.press("Enter")
                page.wait_for_function(
                    """() => document.querySelector('[data-term-xterm]')
                             .textContent.includes('tny exited')""",
                    timeout=30000)

                # the key must never appear in the DOM
                print("stage: quit", flush=True)
                content = page.content()
                assert "test-key-not-real" not in content, "api key leaked into DOM"
                assert not errors, f"page errors: {errors}"

                # named provider pairs in the hash (docs/adr/0018): the CLI's
                # own sole-pair detection must select the provider and run a
                # turn on it, no OPENAI_* anywhere
                page2 = browser.new_page()
                page2.on("pageerror", lambda e: errors.append(str(e)))
                page2.goto(
                    f"http://127.0.0.1:{sport}/index.html"
                    f"#OPENCODE_API_KEY=named-key-not-real"
                    f"&OPENCODE_BASE_URL=http://127.0.0.1:{mport}/v1")
                page2.wait_for_function(
                    """() => document.querySelector('[data-term-xterm]')
                          && document.querySelector('[data-term-xterm]')
                             .textContent.includes('opencode')""",
                    timeout=30000)
                page2.click("[data-term-xterm]")
                page2.keyboard.type("list files in .")
                page2.keyboard.press("Enter")
                page2.wait_for_function(
                    """() => document.querySelector('[data-term-xterm]')
                             .textContent.includes('MOCK-OK')""",
                    timeout=30000)
                print("stage: named-provider turn", flush=True)
                assert "named-key-not-real" not in page2.content(), "named key leaked"
                assert not errors, f"page errors: {errors}"
                browser.close()
        finally:
            mock.terminate()
            mock.wait(timeout=5)
            httpd.shutdown()
    print("test_site_wasm: all assertions passed")


if __name__ == "__main__":
    main()
