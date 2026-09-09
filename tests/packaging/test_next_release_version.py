#!/usr/bin/env python3
"""Automatic release tagging: version arithmetic and the auto-release contract."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "next_release_version.py"
spec = importlib.util.spec_from_file_location("next_release_version", SCRIPT)
assert spec is not None and spec.loader is not None
nrv = importlib.util.module_from_spec(spec)
# dataclasses resolve string annotations through sys.modules[__module__]
sys.modules[spec.name] = nrv
spec.loader.exec_module(nrv)


class PureVersionTests(unittest.TestCase):
    def test_only_stable_tags_parse(self) -> None:
        self.assertEqual(nrv.parse_stable_tag("v1.2.3"), nrv.Version(1, 2, 3))
        for tag in ("v1.2.3-rc.1", "v01.2.3", "1.2.3", "v1.2", "vnext", "v1.2.3+meta"):
            self.assertIsNone(nrv.parse_stable_tag(tag), tag)

    def test_highest_stable_orders_numerically(self) -> None:
        self.assertEqual(
            nrv.highest_stable(["v0.9.0", "v0.10.0", "v0.10.0-rc.1", "v0.2.11"]),
            nrv.Version(0, 10, 0),
        )
        self.assertIsNone(nrv.highest_stable(["v1.0.0-a.1", "nightly"]))

    def test_conventional_prefixes_pick_the_bump(self) -> None:
        self.assertEqual(nrv.bump_kind_for_message("feat: add thing"), nrv.MINOR)
        self.assertEqual(nrv.bump_kind_for_message("feat(tui): add thing"), nrv.MINOR)
        self.assertEqual(nrv.bump_kind_for_message("fix: bound shutdown"), nrv.PATCH)
        self.assertEqual(
            nrv.bump_kind_for_message("optimiser config improvements"), nrv.PATCH
        )
        self.assertEqual(nrv.bump_kind_for_message("feat!: drop flag"), nrv.MAJOR)
        self.assertEqual(
            nrv.bump_kind_for_message("refactor(core)!: rename"), nrv.MAJOR
        )
        self.assertEqual(
            nrv.bump_kind_for_message("fix: x\n\nBREAKING CHANGE: y"), nrv.MAJOR
        )
        self.assertEqual(
            nrv.bump_kind_for_message("fix: x\n\nBREAKING-CHANGE: y"), nrv.MAJOR
        )
        # A "feature" mentioned mid-subject is not a prefix.
        self.assertEqual(
            nrv.bump_kind_for_message("docs: describe feat: syntax"), nrv.PATCH
        )

    def test_highest_bump_across_messages_wins(self) -> None:
        self.assertEqual(nrv.bump_kind_for_messages([]), nrv.PATCH)
        self.assertEqual(
            nrv.bump_kind_for_messages(["ci: x", "feat: y", "fix: z"]), nrv.MINOR
        )
        self.assertEqual(nrv.bump_kind_for_messages(["feat: y", "fix!: z"]), nrv.MAJOR)

    def test_bump_arithmetic(self) -> None:
        v = nrv.Version
        self.assertEqual(nrv.bump(v(0, 8, 0), nrv.PATCH), v(0, 8, 1))
        self.assertEqual(nrv.bump(v(0, 8, 3), nrv.MINOR), v(0, 9, 0))
        # Breaking changes stay minor before 1.0.0 and go major after.
        self.assertEqual(nrv.bump(v(0, 8, 3), nrv.MAJOR), v(0, 9, 0))
        self.assertEqual(nrv.bump(v(1, 4, 2), nrv.MAJOR), v(2, 0, 0))
        self.assertEqual(nrv.bump(v(1, 4, 2), nrv.MINOR), v(1, 5, 0))
        with self.assertRaises(ValueError):
            nrv.bump(v(1, 0, 0), "epoch")


class GitRepo:
    """A throwaway repository with deterministic commits and tags."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "test")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "tag.gpgsign", "false")

    def git(self, *args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(self.path), *args],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def commit(self, message: str) -> str:
        self.git("commit", "-q", "--allow-empty", "-m", message)
        return self.git("rev-parse", "HEAD")

    def tag(self, name: str, ref: str = "HEAD") -> None:
        self.git("tag", "-a", "-m", name, name, ref)

    def merge_branch(self, name: str, messages: list[str], merge_message: str) -> str:
        base = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "-b", name, base)
        for message in messages:
            self.commit(message)
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", merge_message, name)
        return self.git("rev-parse", "HEAD")


class PlanReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = GitRepo(Path(self.tmp.name))
        self.repo.commit("feat: initial")
        self.repo.tag("v0.8.0")

    def plan(self, ref: str = "HEAD", **kwargs: object) -> object:
        return nrv.plan_release(nrv.Repo(self.repo.path), ref, **kwargs)

    def test_already_released_commit_is_skipped(self) -> None:
        plan = self.plan()
        self.assertIsNone(plan.tag)
        self.assertIn("already released as v0.8.0", plan.reason)

    def test_fix_since_last_release_bumps_patch(self) -> None:
        self.repo.merge_branch("fix/x", ["fix: something"], "Merge pull request #1")
        plan = self.plan()
        self.assertEqual(
            (plan.tag, plan.bump, plan.base), ("v0.8.1", nrv.PATCH, "v0.8.0")
        )

    def test_feature_on_a_merged_branch_bumps_minor(self) -> None:
        # The conventional prefix lives on the branch commit, not the merge.
        self.repo.merge_branch(
            "feat/x", ["fix: a", "feat: b"], "Merge pull request #2 from feat/x"
        )
        self.assertEqual(self.plan().tag, "v0.9.0")

    def test_breaking_change_is_minor_before_one_point_zero(self) -> None:
        self.repo.commit("refactor!: drop the old flag")
        self.assertEqual(self.plan().tag, "v0.9.0")

    def test_breaking_change_is_major_after_one_point_zero(self) -> None:
        self.repo.commit("feat: 1.0")
        self.repo.tag("v1.0.0")
        self.repo.commit("fix: x\n\nBREAKING CHANGE: y")
        self.assertEqual(self.plan().tag, "v2.0.0")

    def test_override_forces_the_bump(self) -> None:
        self.repo.commit("docs: note")
        self.assertEqual(self.plan(override=nrv.MINOR).tag, "v0.9.0")

    def test_prerelease_tags_are_neither_bases_nor_collisions(self) -> None:
        self.repo.commit("feat: candidate")
        self.repo.tag("v0.9.0-rc.1")
        self.repo.commit("fix: polish")
        self.assertEqual(self.plan().tag, "v0.9.0")

    def test_stale_commit_behind_a_newer_release_is_skipped(self) -> None:
        old = self.repo.commit("fix: lands first, finishes CI last")
        self.repo.commit("feat: released meanwhile")
        self.repo.tag("v0.9.0")
        plan = self.plan(old)
        self.assertIsNone(plan.tag)
        self.assertIn("v0.9.0 is newer", plan.reason)

    def test_skip_marker_on_the_merged_commits_opts_out(self) -> None:
        self.repo.merge_branch(
            "chore/x", ["ci: tweak [skip release]"], "Merge pull request #3"
        )
        plan = self.plan()
        self.assertIsNone(plan.tag)
        self.assertIn("[skip release]", plan.reason)

    def test_skip_marker_on_an_earlier_merge_does_not_block_later_ones(self) -> None:
        self.repo.merge_branch("chore/x", ["ci: tweak [skip release]"], "Merge #3")
        self.repo.merge_branch("fix/y", ["fix: real"], "Merge #4")
        self.assertEqual(self.plan().tag, "v0.8.1")

    def test_skip_marker_on_a_squash_commit_opts_out(self) -> None:
        self.repo.commit("chore: bump [no release]")
        self.assertIsNone(self.plan().tag)

    def test_first_release_without_any_tag(self) -> None:
        self.repo.git("tag", "-d", "v0.8.0")
        self.repo.commit("fix: more")
        plan = self.plan()
        self.assertEqual((plan.tag, plan.base), ("v0.1.0", None))

    def test_cli_writes_github_outputs_and_prints_the_tag(self) -> None:
        self.repo.commit("feat: x")
        output = Path(self.tmp.name) / "outputs"
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--repo",
                str(self.repo.path),
                "--github-output",
                str(output),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.stdout, "v0.9.0\n")
        lines = output.read_text(encoding="utf-8").splitlines()
        self.assertIn("tag=v0.9.0", lines)
        self.assertIn("bump=minor", lines)
        self.assertIn("base=v0.8.0", lines)

    def test_cli_skip_prints_nothing_on_stdout(self) -> None:
        output = Path(self.tmp.name) / "outputs"
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--repo",
                str(self.repo.path),
                "--github-output",
                str(output),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.stdout, "")
        self.assertIn("skip:", result.stderr)
        self.assertIn("tag=\n", output.read_text(encoding="utf-8"))


class AutoReleaseWorkflowContractTests(unittest.TestCase):
    """The workflow must gate on every main-branch check and dispatch release.yml."""

    workflow = (ROOT / ".github/workflows/auto-release.yml").read_text(encoding="utf-8")

    def test_triggers_on_every_main_gate(self) -> None:
        self.assertIn("workflows: [ci, nix, sdk]", self.workflow)
        self.assertIn("types: [completed]", self.workflow)
        self.assertIn("branches: [main]", self.workflow)

    def test_requires_all_gates_green_before_tagging(self) -> None:
        for path in (
            ".github/workflows/ci.yml",
            ".github/workflows/nix.yml",
            ".github/workflows/sdk.yml",
        ):
            self.assertIn(path, self.workflow)
        self.assertIn(
            "github.event.workflow_run.conclusion == 'success'", self.workflow
        )

    def test_dispatches_the_release_workflow_on_the_tag(self) -> None:
        # Tags pushed with GITHUB_TOKEN never start `on: push: tags`; the
        # documented fallback is a dispatch on the tag ref.
        self.assertIn('gh workflow run release.yml --ref "$tag"', self.workflow)
        self.assertIn("scripts/next_release_version.py", self.workflow)
        self.assertIn("concurrency:\n  group: auto-release", self.workflow)


if __name__ == "__main__":
    unittest.main()
