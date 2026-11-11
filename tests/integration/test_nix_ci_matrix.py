#!/usr/bin/env python3
"""CI targets Linux/macOS; Nix remains an optional developer workflow (ADR 0137)."""

from __future__ import annotations

import ast
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_SYSTEMS = ["x86_64-linux", "aarch64-linux", "aarch64-darwin"]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def package_check_files() -> list[Path]:
    """The files of nix/source.nix's packageChecks fileset."""
    block = re.search(
        r"(?ms)^  packageChecks = toSource \{.*?unions \[(.*?)\];",
        read("nix/source.nix"),
    )
    assert block, "nix/source.nix has no packageChecks fileset"
    return [ROOT / name for name in re.findall(r"\.\./(\S+)", block.group(1))]


def check_package_checks_are_self_contained() -> None:
    """The installed-package check runs from a tree holding only this fileset.

    A module-level import of a sibling module that the fileset omits fails the
    Nix build, and no CI job runs Nix. Imports inside functions are left to the
    platform branch that runs them.
    """
    files = package_check_files()
    assert files, "packageChecks fileset is empty"
    for path in files:
        assert path.is_file(), path
        for node in ast.parse(path.read_text(encoding="utf-8")).body:
            if isinstance(node, ast.Import):
                modules = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom) and node.level == 0:
                modules = [node.module]
            else:
                continue
            for module in modules:
                sibling = path.with_name(module.split(".")[0] + ".py")
                assert not sibling.is_file() or sibling in files, (
                    f"{path.name} imports {sibling.name}; "
                    "add it to nix/source.nix packageChecks"
                )


def main() -> int:
    check_package_checks_are_self_contained()
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
    tagging = read(".github/workflows/tag-release.yml")
    for text in (ci, release):
        for runner in ("ubuntu-24.04", "ubuntu-24.04-arm", "macos-15"):
            assert runner in text, runner
        assert "needs.windows" not in text
        assert "  windows:" not in text
        assert "continue-on-error:" not in text
    # Workflow configuration is executable policy: every declared check must
    # participate in the release aggregate, independent of list ordering.
    aggregate = re.search(r"(?ms)^  ci:\n(.*?)(?=^  [\w-]+:|\Z)", ci)
    assert aggregate, "missing ci release gate"
    gate = aggregate.group(1)
    dependency_list = re.search(r"needs:\s*\[([^]]+)\]", gate)
    assert dependency_list, "ci dependencies must be explicit"
    dependencies = {name.strip() for name in dependency_list.group(1).split(",")}
    jobs = set(re.findall(r"(?m)^  ([\w-]+):\s*$", ci.split("\njobs:\n", 1)[1])) - {
        "ci"
    }
    assert dependencies == jobs, ("ungated CI jobs", jobs - dependencies)
    assert "if: always()" in gate, "failed dependencies must not skip the gate"
    for name in dependencies:
        assert f'test "${{{{ needs.{name}.result }}}}" = "success"' in gate, (
            "dependency result not enforced",
            name,
        )
    assert "needs: [build, musl, validate-registries]" in release
    assert "workflows: [ci, sdk]" in auto
    assert ".github/workflows/nix.yml" not in auto
    assert "nix" not in re.search(r"for path in (.*?); do", tagging).group(1)
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
    print(
        "test_nix_ci_matrix: Linux/macOS CI, ci+sdk release gates, optional local Nix"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
