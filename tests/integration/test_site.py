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
    term = (SITE / "assets" / "term.js").read_text(encoding="utf-8")

    for needle in (
        'id="tny-term"',
        'data-term-input',
        'data-term-transcript',
        "assets/term-core.js",
        "assets/term.js",
        "takeSecretsFromLocation",
        'name="referrer" content="no-referrer"',
    ):
        if needle not in html:
            fail(f"index.html missing {needle!r}")

    if 'role="img"' in html:
        fail("landing terminal is still a static role=img mock")

    if "sk-" in html and "sk-proj" in html:
        fail("landing HTML looks like it baked in a live key")

    for needle in ("term-input", "term-transcript", "term-overlay"):
        if needle not in css:
            fail(f"site.css missing {needle}")

    for needle in (
        "aesGcmSeal",
        "obfuscateUrl",
        "SseParser",
        "OPENAI_BASE_URL",
        "OPENAI_API_KEY",
    ):
        if needle not in core:
            fail(f"term-core.js missing {needle}")

    if "indexedDB" not in term or '"/responses"' not in term:
        fail("term.js does not look like a client-side agent loop")
    if "chat/completions" in term:
        fail("term.js regressed to the legacy chat wire (docs/adr/0014)")
    if '"store": false' not in term and "store: false" not in term:
        fail("term.js must send store:false — the tab owns session state")
    if "localStorage" in term and "OPENAI_API_KEY" in term:
        # plaintext key must not be written to localStorage
        fail("term.js appears to persist the key in localStorage")

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
