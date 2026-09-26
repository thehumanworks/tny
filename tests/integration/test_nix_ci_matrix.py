#!/usr/bin/env python3
"""CI targets Linux/macOS; Nix remains an optional developer workflow (ADR 0137)."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_SYSTEMS = ["x86_64-linux", "aarch64-linux", "aarch64-darwin"]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    workflows = ROOT / ".github/workflows"
    assert not (workflows / "nix.yml").exists(), "Nix must not be a CI workflow"
    for path in workflows.glob("*.yml"):
        text = path.read_text(encoding="utf-8")
        assert "windows-" not in text, path
        assert "msys2/setup-msys2" not in text, path
        assert not re.search(r"nix (?:build|flake check)|install-nix-action", text), (
            path
        )
        for runner in re.findall(r"(?m)^\s+runs-on:\s*(.+)$", text):
            assert runner.startswith(("ubuntu-", "macos-", "${{ matrix.os }}")), (
                path,
                runner,
            )

    ci = read(".github/workflows/ci.yml")
    release = read(".github/workflows/release.yml")
    auto = read(".github/workflows/auto-release.yml")
    for text in (ci, release):
        for runner in ("ubuntu-24.04", "ubuntu-24.04-arm", "macos-15"):
            assert runner in text, runner
        assert "needs.windows" not in text
        assert "  windows:" not in text
        assert "continue-on-error:" not in text
    assert "needs: [quality, build, musl, wasm, tsan, fuzz, lean-proofs]" in ci
    assert "needs: [build, musl, validate-registries]" in release
    assert "workflows: [ci, sdk]" in auto
    assert ".github/workflows/nix.yml" not in auto
    assert "nix" not in re.search(r"for path in (.*?); do", auto).group(1)
    assert "nix flake check" not in auto
    # Removing Nix must not drop the only actual ImageMagick 7 conversion run.
    assert "magick -version" in ci
    assert 'TNY="$PWD/build/tny" python3 tests/integration/test_image_exports.py' in ci

    flake = read("flake.nix")
    block = re.search(r"(?ms)systems\s*=\s*\[(.*?)\];", flake)
    assert block
    assert sorted(re.findall(r'"([^"]+)"', block.group(1))) == sorted(EXPECTED_SYSTEMS)
    for path in ("default.nix", "shell.nix", "nix/devshell.nix", "nix/tests.nix"):
        assert (ROOT / path).is_file(), path
    assert "checks = forAllSystems" in flake
    assert "packages = forAllSystems" in flake
    assert "../.github/workflows" in read("nix/source.nix")
    for path in ("docs/ci.md", "docs/nix.md"):
        text = read(path)
        assert "optional developer" in text, path
        assert "nix flake check" in text, path
        assert ".github/workflows/nix.yml" not in text, path
    print(
        "test_nix_ci_matrix: Linux/macOS CI, ci+sdk release gates, optional local Nix"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
