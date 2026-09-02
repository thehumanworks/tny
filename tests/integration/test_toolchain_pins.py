#!/usr/bin/env python3
"""Keep .mise.toml and the CI quality job pinned to the same tool versions.

`mise install && make quality` (docs/adr/0061) is only worth documenting while
the versions a developer gets are the versions CI enforces: clang-format's
output changes between majors, and a drifting pin turns a green local run into
a red CI job. This check reads both files and fails on any mismatch.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MISE = ROOT / ".mise.toml"
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
CI_DOC = ROOT / "docs" / "ci.md"

# mise tool name -> regex capturing the same version out of ci.yml.
PINS = {
    '"pipx:clang-format"': r"pipx install clang-format==([0-9.]+)",
    '"pipx:clang-tidy"': r"pipx install clang-tidy==([0-9.]+)",
    "ruff": r"pipx install ruff==([0-9.]+)",
    "shfmt": r"go install mvdan\.cc/sh/v3/cmd/shfmt@v([0-9.]+)",
    "actionlint": r"go install github\.com/rhysd/actionlint/cmd/actionlint@v([0-9.]+)",
}

# Tools mise pins that the CI runner already ships; they still have to be in
# .mise.toml so a developer machine gets them.
REQUIRED_MISE_TOOLS = ["shellcheck", "python", "node", "uv"]


def _require_file(path: Path) -> str:
    if not path.is_file():
        raise SystemExit(
            f"{path.relative_to(ROOT)} is missing from the test source; "
            "add it to nix/source.nix testFiles"
        )
    return path.read_text(encoding="utf-8")


def _mise_pins(text: str) -> dict[str, str]:
    pins: dict[str, str] = {}
    for line in text.splitlines():
        match = re.match(r'^\s*("?[A-Za-z0-9_:.-]+"?)\s*=\s*"([^"]+)"\s*$', line)
        if match:
            pins[match.group(1)] = match.group(2)
    return pins


def main() -> int:
    mise = _mise_pins(_require_file(MISE))
    workflow = _require_file(WORKFLOW)
    ci_doc = _require_file(CI_DOC)
    failures: list[str] = []

    for tool, pattern in PINS.items():
        found = re.search(pattern, workflow)
        if not found:
            failures.append(f"ci.yml no longer pins {tool} (pattern {pattern!r})")
            continue
        if tool not in mise:
            failures.append(f".mise.toml does not pin {tool}")
        elif mise[tool] != found.group(1):
            failures.append(
                f"{tool}: .mise.toml pins {mise[tool]}, ci.yml pins {found.group(1)}"
            )

    for tool in REQUIRED_MISE_TOOLS:
        if tool not in mise:
            failures.append(f".mise.toml does not pin {tool}")

    if "mise install" not in ci_doc:
        failures.append("docs/ci.md does not document `mise install`")

    if "make valgrind" not in workflow:
        failures.append("ci.yml has no valgrind job running `make valgrind`")

    if failures:
        for problem in failures:
            print(f"toolchain pins: {problem}")
        return 1

    print(f"toolchain pins: {len(PINS) + len(REQUIRED_MISE_TOOLS)} tools in lockstep")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
