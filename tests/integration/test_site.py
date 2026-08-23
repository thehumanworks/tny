#!/usr/bin/env python3
"""Check the generated GitHub Pages landing terminal and its JS helpers."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "site"
BUILD = ROOT / "scripts" / "site_build.py"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    subprocess.check_call([sys.executable, str(BUILD)], cwd=ROOT)
    html = (SITE / "index.html").read_text(encoding="utf-8")
    css = (SITE / "assets" / "site.css").read_text(encoding="utf-8")
    core = (SITE / "assets" / "term-core.js").read_text(encoding="utf-8")
    wasm_boot = (SITE / "assets" / "term-wasm.js").read_text(encoding="utf-8")

    for needle in (
        'id="tny-term"',
        "data-term-xterm",
        "assets/term-core.js",
        "assets/vendor/xterm.js",
        "assets/term-wasm.js",
        "takeSecretsFromLocation",
        'name="referrer" content="no-referrer"',
        "viewport-fit=cover",
        "interactive-widget=resizes-content",
    ):
        if needle not in html:
            fail(f"index.html missing {needle!r}")

    if "assets/term.js" in html:
        fail("index.html still loads the deleted JS agent loop (docs/adr/0017)")

    if 'role="img"' in html:
        fail("landing terminal is still a static role=img mock")

    if "sk-" in html and "sk-proj" in html:
        fail("landing HTML looks like it baked in a live key")

    for needle in (
        "term-xterm",
        "term-main--wasm",
        "safe-area-inset",
        "kb-open",
        "min-height: 44px",
        "xterm-helper-textarea",
    ):
        if needle not in css:
            fail(f"site.css missing {needle}")

    for needle in (
        "aesGcmSeal",
        "obfuscateUrl",
        "SseParser",
        "OPENAI_BASE_URL",
        "OPENAI_API_KEY",
        "wrapToCols",
        "proposeTermGeometry",
    ):
        if needle not in core:
            fail(f"term-core.js missing {needle}")

    if (SITE / "assets" / "term.js").exists():
        fail("site/assets/term.js still exists — the hand-written agent loop "
             "was deleted; the loop is the wasm binary (docs/adr/0017)")

    # No agent loop may live in site JS: the CI-tested wasm artifact is the
    # only thing on this page that speaks a provider wire.
    for js in sorted((SITE / "assets").glob("*.js")):
        blob = js.read_text(encoding="utf-8")
        for needle in ("chat/completions", '"/responses"', "'/responses'"):
            if needle in blob:
                fail(f"{js.name} contains provider-wire code ({needle!r}); "
                     "the loop belongs to the wasm binary alone")

    for needle in (
        "wasm/tny-web.mjs",
        "sanitizeApiKey",
        "callMain",
        "proposeTermGeometry",
        "visualViewport",
        "data-term-cols",
    ):
        if needle not in wasm_boot:
            fail(f"term-wasm.js missing {needle}")
    if "localStorage" in wasm_boot and "OPENAI_API_KEY" in wasm_boot:
        fail("term-wasm.js appears to persist the key in localStorage")

    node = subprocess.run(["node", "-v"], capture_output=True, text=True)
    if node.returncode != 0:
        fail("node is required to unit-test term-core.js")
    js = subprocess.run(
        ["node", str(ROOT / "tests" / "site" / "test_term.js")],
        cwd=ROOT,
    )
    if js.returncode != 0:
        sys.exit(js.returncode)
    print("ok site landing terminal")


if __name__ == "__main__":
    main()
