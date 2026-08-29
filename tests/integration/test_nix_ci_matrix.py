#!/usr/bin/env python3
"""Keep the documented Nix systems and native CI matrix in lockstep."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "nix.yml"
CI_DOC = ROOT / "docs" / "ci.md"
NIX_DOC = ROOT / "docs" / "nix.md"
FLAKE = ROOT / "flake.nix"

EXPECTED_MATRIX = [
    ("nix-linux-x86_64", "ubuntu-24.04"),
    ("nix-linux-aarch64", "ubuntu-24.04-arm"),
    ("nix-darwin-arm64", "macos-15"),
]
EXPECTED_SYSTEMS = ["x86_64-linux", "aarch64-linux", "aarch64-darwin"]


def main() -> int:
    workflow = WORKFLOW.read_text(encoding="utf-8")
    ci_doc = CI_DOC.read_text(encoding="utf-8")
    nix_doc = NIX_DOC.read_text(encoding="utf-8")
    flake = FLAKE.read_text(encoding="utf-8")

    matrix = re.search(r"(?ms)^      matrix:\n(?P<body>.*?)(?=^    steps:)", workflow)
    assert matrix, "nix workflow check matrix not found"
    entries = re.findall(
        r"(?m)^          - name: ([^\n]+)\n            os: ([^\n]+)$",
        matrix.group("body"),
    )
    assert entries == EXPECTED_MATRIX, entries

    assert workflow.count("\njobs:") == 1
    assert workflow.count("\n  check:") == 1
    assert "continue-on-error:" not in workflow
    assert re.findall(r"(?m)^\s+if:\s*(.+)$", workflow) == ["runner.os == 'macOS'"]
    assert 'if [ "$(uname -m)" != "arm64" ]; then' in workflow
    assert "x86_64-darwin" not in workflow

    for command in (
        "nix flake check --print-build-logs",
        "nix build .#tny --print-build-logs",
        "./result/bin/tny --version",
        "./result/bin/tny --help",
        "./result/bin/tny doctor --json",
        'test "$got" = "$want"',
        "nix-instantiate default.nix -A tny",
        "nix-instantiate default.nix -A libtny",
        "nix-instantiate shell.nix",
    ):
        assert command in workflow, command

    systems_block = re.search(r"(?ms)systems\s*=\s*\[(?P<body>.*?)\];", flake)
    assert systems_block, "flake systems list not found"
    systems = re.findall(r'"([^"]+)"', systems_block.group("body"))
    assert sorted(systems) == sorted(EXPECTED_SYSTEMS), systems

    for runner, system in (
        ("ubuntu-24.04", "x86_64-linux"),
        ("ubuntu-24.04-arm", "aarch64-linux"),
        ("macos-15", "aarch64-darwin"),
    ):
        assert runner in ci_doc, runner
        assert system in ci_doc, system
        assert runner in nix_doc, runner
        assert system in nix_doc, system

    for text in (".github/workflows/nix.yml", "nix flake check"):
        assert text in ci_doc, text
    for text in (
        "packages.tny",
        "packages.libtny",
        "checks.tests",
        "publishes no artifact",
    ):
        assert text in ci_doc, text
    assert ".github/workflows/nix.yml" in nix_doc
    assert "native-checks all three systems" in nix_doc

    banned_phrases = (
        "on `ubuntu-24.04` and `macos-15`",
        "ubuntu-24.04 and macos-15 only",
        "only x86_64 Linux + Darwin",
        "only x86_64 Linux and Darwin",
    )
    for phrase in banned_phrases:
        assert phrase not in ci_doc, phrase
        assert phrase not in nix_doc, phrase

    print("test_nix_ci_matrix: all assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
