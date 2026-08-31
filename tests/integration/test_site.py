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
    workflows = (SITE / "docs" / "workflows.html").read_text(encoding="utf-8")
    size_page = (SITE / "docs" / "size.html").read_text(encoding="utf-8")
    llms = (SITE / "llms.txt").read_text(encoding="utf-8")

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

    for needle in ("<span>v0.3.0</span>", "<span>0.68mib</span>"):
        if needle not in html:
            fail(f"release metadata missing from index.html: {needle!r}")
    if "715,744 B (0.68 MiB)" not in size_page:
        fail("size page does not identify the measured v0.3.0 binary")
    for needle in ("Version: 0.3.0", "715,744 bytes (~0.68 MiB)"):
        if needle not in llms:
            fail(f"llms.txt release metadata missing {needle!r}")

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
        fail(
            "site/assets/term.js still exists — the hand-written agent loop "
            "was deleted; the loop is the wasm binary (docs/adr/0017)"
        )

    # No agent loop may live in site JS: the CI-tested wasm artifact is the
    # only thing on this page that speaks a provider wire.
    for js in sorted((SITE / "assets").glob("*.js")):
        blob = js.read_text(encoding="utf-8")
        for needle in ("chat/completions", '"/responses"', "'/responses'"):
            if needle in blob:
                fail(
                    f"{js.name} contains provider-wire code ({needle!r}); "
                    "the loop belongs to the wasm binary alone"
                )

    for needle in (
        "wasm/tny-web.mjs",
        "sanitizeApiKey",
        "callMain",
        "convertEol: true",
        "proposeTermGeometry",
        "visualViewport",
        "data-term-cols",
        "empty skips",
    ):
        if needle not in wasm_boot:
            fail(f"term-wasm.js missing {needle}")
    if "convertEol: false" in wasm_boot:
        fail("term-wasm.js leaves raw wasm LF output at the previous column")
    if "localStorage" in wasm_boot and "OPENAI_API_KEY" in wasm_boot:
        fail("term-wasm.js appears to persist the key in localStorage")

    workflow_example = """tny_task implement \\
  --after architecture --after tests --no-context --provider codex -- \\
  \"Implement from the architecture report after both reviews finish\""""
    if workflow_example not in workflows:
        fail("workflows.html lost shell line continuations")

    workflow_url = "https://thehumanworks.github.io/tny/docs/workflows.html"
    tnytty_urls = (
        "https://thehumanworks.github.io/tny/docs/tnytty.html",
        "https://thehumanworks.github.io/tny/docs/tnytty-cli.html",
        "https://thehumanworks.github.io/tny/docs/tnytty-config.html",
        "https://thehumanworks.github.io/tny/docs/tnytty-api.html",
        "https://thehumanworks.github.io/tny/docs/tnytty-architecture.html",
    )
    for sitemap in (SITE / "sitemap.xml", ROOT / "docs" / "sitemap.xml"):
        text = sitemap.read_text(encoding="utf-8")
        if workflow_url not in text:
            fail(f"{sitemap.relative_to(ROOT)} is missing the workflows page")
        for url in tnytty_urls:
            if url not in text:
                fail(f"{sitemap.relative_to(ROOT)} is missing {url}")

    tnytty = (SITE / "docs" / "tnytty.html").read_text(encoding="utf-8")
    for needle in (
        "the tiny terminal",
        "tnytty run --listen",
        'href="tnytty-api.html"',
        'aria-current="page">tnytty</a>',
    ):
        if needle not in tnytty:
            fail(f"docs/tnytty.html missing {needle!r}")
    api = (SITE / "docs" / "tnytty-api.html").read_text(encoding="utf-8")
    for needle in ("Authorization: Bearer", "/v1/sessions", "input queue full"):
        if needle not in api:
            fail(f"docs/tnytty-api.html missing {needle!r}")
    if "tnytty.html" not in (SITE / "docs" / "index.html").read_text(encoding="utf-8"):
        fail("docs/index.html lost the tnytty sidebar link")

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
