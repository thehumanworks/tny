#!/usr/bin/env python3
"""Layout smoke for the GitHub Pages landing page on a phone-sized viewport.

Does not need the wasm artifact: the pre-launch intro paints in xterm.js
alone. Skips (exit 0) when Playwright/Chromium is missing so the
fixture-only suite stays runnable everywhere.
"""
from __future__ import annotations

import http.server
import os
import socket
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SITE = os.path.join(ROOT, "site")

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("test_site_mobile: skip (playwright not installed)")
    raise SystemExit(0)


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def chromium_exe() -> str | None:
    for path in (
        os.environ.get("TNY_CHROMIUM"),
        "/opt/pw-browsers/chromium",
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
    ):
        if path and os.path.exists(path):
            return path
    return None


def main() -> None:
    port = free_port()

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=SITE, **kw)

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), Quiet)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as pw:
            exe = chromium_exe()
            browser = (
                pw.chromium.launch(executable_path=exe)
                if exe
                else pw.chromium.launch()
            )
            errors = []

            def check_phone(width, height, color_scheme):
                page = browser.new_page(viewport={"width": width, "height": height})
                page.on("pageerror", lambda e: errors.append(f"{width}x{height}: {e}"))
                page.emulate_media(color_scheme=color_scheme)
                page.goto(f"http://127.0.0.1:{port}/index.html")
                page.wait_for_selector("[data-term-xterm] .xterm-rows", timeout=15000)

                cols = int(page.get_attribute("[data-term-xterm]", "data-term-cols") or "0")
                assert 2 <= cols < 80, f"{width}x{height}: fitted cols={cols}"

                geom = page.evaluate(
                    """() => {
                      const mount = document.querySelector('[data-term-xterm]');
                      const screen = document.querySelector('.xterm-screen');
                      const nav = Array.from(document.querySelectorAll('.nav a'));
                      const install = document.querySelector('.install-line');
                      return {
                        mountW: mount.getBoundingClientRect().width,
                        screenW: screen ? screen.getBoundingClientRect().width : 0,
                        navMinH: Math.min(...nav.map((a) => a.getBoundingClientRect().height)),
                        installWrap: install
                          ? getComputedStyle(install).whiteSpace
                          : "",
                        overflowX: document.documentElement.scrollWidth
                          - document.documentElement.clientWidth,
                      };
                    }"""
                )
                assert geom["screenW"] <= geom["mountW"] + 2, (width, geom)
                assert geom["navMinH"] >= 40, (width, geom)
                assert geom["installWrap"] == "normal", (width, geom)
                assert geom["overflowX"] <= 2, (width, geom)

                text = page.evaluate(
                    """() => Array.from(document.querySelectorAll(
                         '[data-term-xterm] .xterm-rows > div'))
                         .map((r) => r.textContent).join('\\n')"""
                )
                assert "tny" in text, (width, text)
                assert "provider" in text, (width, text)
                if width <= 320:
                    assert "empty skips" in text, (width, text)
                page.close()

            check_phone(390, 844, "light")
            check_phone(320, 568, "dark")
            assert not errors, f"page errors: {errors}"
            browser.close()
    finally:
        httpd.shutdown()
    print("test_site_mobile: phone viewport fits without clipping")


if __name__ == "__main__":
    main()
