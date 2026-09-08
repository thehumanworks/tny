#!/usr/bin/env python3
"""Real Git repositories and PTYs: worktree lifecycle without any live provider."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_tui import BANNER, MOCK, TNY, Term, base_env, clean, free_port

TNY = str(Path(TNY).resolve())


class Worktrees(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-wt-", dir="/tmp")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.home = self.root / "home"
        self.home.mkdir()
        self.repo = self.root / "repo with spaces"
        self.env = base_env(str(self.home), {"TNY_ISOLATE": "0"})
        self.init_repo(self.repo)
        self.worktrees = self.home / ".tny" / "worktrees"

    def git(self, path, *args, check=True):
        p = subprocess.run(
            ["git", "-C", str(path), *args],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=20,
        )
        if check:
            self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout.strip()

    def init_repo(self, path):
        path.mkdir()
        self.git(path, "init", "-b", "main")
        self.git(path, "config", "user.name", "Worktree Test")
        self.git(path, "config", "user.email", "worktree@example.invalid")
        (path / "file.txt").write_text("base\n")
        (path / ".gitignore").write_text("ignored.txt\n")
        (path / "AGENTS.md").write_text("WORKTREE-INSTRUCTIONS\n")
        self.commit(path)

    def commit(self, path):
        self.git(path, "add", ".")
        self.git(path, "commit", "-qm", "fixture")

    def cli(self, *args, cwd=None, env=None, ok=True, input=""):
        p = subprocess.run(
            [TNY, *args],
            cwd=cwd or self.repo,
            env=env or self.env,
            input=input,
            text=True,
            capture_output=True,
            timeout=30,
        )
        self.assertEqual(p.returncode == 0, ok, p.stderr + p.stdout)
        return p

    def enter(self, name="feature", cwd=None):
        p = self.cli("--worktree=" + name, "status", "--json", cwd=cwd)
        w = self.worktrees / name
        self.assertEqual(json.loads(p.stdout)["workspace"], str(w))
        return w

    def term(self, name=None, extra=(), env=None):
        flags = ["--worktree=" + name] if name else []
        t = Term([TNY, *flags, *extra], env or self.env, str(self.repo))
        self.addCleanup(t.close)
        t.expect(BANNER)
        return t

    def finish(self, t, choice="\r", quit_command="/quit\r", rc=0):
        t.send(quit_command)
        t.expect("[K]eep (default):")
        t.send(choice)
        self.assertEqual(t.wait(), rc, clean(t.buf))
        self.assertTrue(t.restored())

    def test_create_from_nested_dirty_head_and_reuse(self):
        nested = self.repo / "nested"
        nested.mkdir()
        (self.repo / "file.txt").write_text("uncommitted source\n")
        head = self.git(self.repo, "rev-parse", "HEAD")
        w = self.enter(cwd=nested)
        self.assertEqual(self.git(w, "branch", "--show-current"), "worktree/feature")
        self.assertEqual(self.git(w, "rev-parse", "HEAD"), head)
        self.assertEqual((w / "file.txt").read_text(), "base\n")
        (w / "own.txt").write_text("keep this\n")
        self.assertEqual(self.enter(), w)
        self.assertEqual((w / "own.txt").read_text(), "keep this\n")
        self.assertEqual((self.repo / "file.txt").read_text(), "uncommitted source\n")

    def test_random_cwd_flag_and_reserved_name(self):
        for _ in range(2):
            p = self.cli(
                "--cwd", str(self.repo), "--worktree", "status", "--json", cwd=self.home
            )
            w = Path(json.loads(p.stdout)["workspace"])
            self.assertEqual(w.parent, self.worktrees)
        self.assertEqual(len(list(self.worktrees.iterdir())), 2)
        self.enter("status")

    def test_reject_non_git_unborn_invalid_names_and_ssh(self):
        p = self.cli("--worktree", cwd=self.home, ok=False)
        self.assertIn("Git working repository", p.stderr)
        unborn = self.root / "unborn"
        unborn.mkdir()
        self.git(unborn, "init")
        self.assertIn(
            "no HEAD commit", self.cli("--worktree", cwd=unborn, ok=False).stderr
        )
        for name in ("", "../outside", "a/b", "two words", "-flag"):
            self.cli("--worktree=" + name, ok=False)
        self.cli("--worktree", "--ssh", "example.invalid", ok=False)
        self.assertFalse(self.worktrees.exists())

    def test_help_never_creates_worktree(self):
        for args in (("--worktree", "--help"), ("--worktree", "ask", "--help")):
            self.assertIn("Usage:", self.cli(*args, cwd=self.home).stdout)
        self.assertFalse(self.worktrees.exists())

    def test_git_environment_cannot_redirect_workspace(self):
        other = self.root / "env-target"
        self.init_repo(other)
        env = {
            **self.env,
            "GIT_DIR": str(other / ".git"),
            "GIT_WORK_TREE": str(other),
            "GIT_INDEX_FILE": str(other / "alternate-index"),
        }
        p = self.cli("--worktree=env", "status", "--json", env=env)
        w = Path(json.loads(p.stdout)["workspace"])
        self.assertEqual(
            self.git(w, "rev-parse", "--git-common-dir"), str(self.repo / ".git")
        )
        self.assertFalse((other / "alternate-index").exists())

    def test_collisions_never_enter_other_repository_or_symlink(self):
        w = self.enter()
        other = self.root / "other"
        self.init_repo(other)
        self.cli("--worktree=feature", cwd=other, ok=False)
        (self.worktrees / "plain").mkdir()
        self.cli("--worktree=plain", ok=False)
        (self.worktrees / "link").symlink_to(w)
        self.cli("--worktree=link", ok=False)
        self.assertTrue(w.is_dir())

    def test_default_reject_escape_and_eof_keep(self):
        for name, choice in (
            ("default", "\r"),
            ("reject", "n"),
            ("escape", "\x1b"),
            ("cancel", "\x03"),
        ):
            with self.subTest(choice=choice):
                t = self.term(name)
                self.finish(t, choice)
                self.assertTrue((self.worktrees / name).is_dir())
        self.cli("--worktree=eof")  # piped EOF must not block for confirmation
        self.assertTrue((self.worktrees / "eof").is_dir())

    def test_merge_commits_and_keep_directory(self):
        t = self.term("merge")
        w = self.worktrees / "merge"
        (w / "merged.txt").write_text("merged\n")
        self.commit(w)
        self.finish(t, "m")
        self.assertEqual((self.repo / "merged.txt").read_text(), "merged\n")
        self.assertEqual(
            self.git(self.repo, "rev-parse", "HEAD"), self.git(w, "rev-parse", "HEAD")
        )
        self.assertTrue(w.is_dir())

    def test_exit_alias_and_ctrl_d_offer_cleanup(self):
        for name, command in (
            ("alias", "/exit\r"),
            ("ctrl-d", "\x04"),
            ("ctrl-c", "\x03\x03"),
        ):
            t = self.term(name)
            self.finish(t, quit_command=command, rc=130 if name == "ctrl-c" else 0)
            self.assertTrue((self.worktrees / name).is_dir())

    def test_slash_non_git_errors_without_changing_workspace(self):
        t = Term([TNY], self.env, str(self.home))
        self.addCleanup(t.close)
        t.expect(BANNER)
        t.send("/worktree nope\r")
        t.expect("Git working repository")
        t.send("/quit\r")
        self.assertEqual(t.wait(), 0)
        self.assertNotIn("[K]eep (default):", clean(t.buf))
        self.assertFalse(self.worktrees.exists())

    def test_merge_uses_recorded_origin_on_reentry(self):
        w = self.enter("original")
        self.git(self.repo, "switch", "-c", "different")
        t = self.term("original")
        self.finish(t, "m", rc=1)
        self.assertIn("different branch", clean(t.buf))
        self.assertTrue(w.is_dir())

    def test_merge_divergence_and_conflicts(self):
        w = self.enter("divergent")
        (self.repo / "source.txt").write_text("source\n")
        self.commit(self.repo)
        (w / "child.txt").write_text("child\n")
        self.commit(w)
        t = self.term("divergent")
        self.finish(t, "m")
        parents = self.git(
            self.repo, "rev-list", "--parents", "-n", "1", "HEAD"
        ).split()
        self.assertEqual(len(parents), 3)
        conflict = self.enter("conflict")
        (self.repo / "file.txt").write_text("source edit\n")
        self.commit(self.repo)
        (conflict / "file.txt").write_text("child edit\n")
        self.commit(conflict)
        t = self.term("conflict")
        self.finish(t, "m", rc=1)
        self.assertTrue(conflict.is_dir())
        self.assertTrue((self.repo / ".git" / "MERGE_HEAD").exists())
        self.assertIn("git merge --abort", clean(t.buf))

    def test_remove_keeps_branch_and_recreates_from_it(self):
        t = self.term("remove")
        w = self.worktrees / "remove"
        (w / "saved.txt").write_text("saved in branch\n")
        self.commit(w)
        head = self.git(w, "rev-parse", "HEAD")
        self.finish(t, "r")
        self.assertFalse(w.exists())
        self.assertEqual(self.git(self.repo, "rev-parse", "worktree/remove"), head)
        self.enter("remove")
        self.assertEqual((w / "saved.txt").read_text(), "saved in branch\n")

    def test_dirty_ignored_untracked_and_locked_removal_refused(self):
        for name, filename in (
            ("dirty", "file.txt"),
            ("untracked", "new.txt"),
            ("ignored", "ignored.txt"),
        ):
            t = self.term(name)
            w = self.worktrees / name
            (w / filename).write_text("do not discard\n")
            self.finish(t, "r", rc=1)
            self.assertEqual((w / filename).read_text(), "do not discard\n")
        t = self.term("locked")
        self.git(self.repo, "worktree", "lock", str(self.worktrees / "locked"))
        self.finish(t, "r", rc=1)

    def test_dirty_source_and_child_refuse_merge(self):
        for name, source in (("source-dirty", True), ("child-dirty", False)):
            t = self.term(name)
            w = self.worktrees / name
            file = (self.repo if source else w) / "file.txt"
            file.write_text("uncommitted\n")
            self.finish(t, "m", rc=1)
            self.assertEqual(file.read_text(), "uncommitted\n")
            file.write_text("base\n")

    def test_merge_never_overwrites_ignored_source_files(self):
        t = self.term("ignored-source")
        w = self.worktrees / "ignored-source"
        (self.repo / "ignored.txt").write_text("local ignored data\n")
        (w / "ignored.txt").write_text("committed child data\n")
        self.git(w, "add", "-f", "ignored.txt")
        self.git(w, "commit", "-qm", "track ignored file")
        head = self.git(self.repo, "rev-parse", "HEAD")
        self.finish(t, "m", rc=1)
        self.assertEqual(
            (self.repo / "ignored.txt").read_text(), "local ignored data\n"
        )
        self.assertEqual(self.git(self.repo, "rev-parse", "HEAD"), head)

    def test_changed_branch_and_unfinished_operation_refuse_removal(self):
        t = self.term("switched")
        w = self.worktrees / "switched"
        self.git(w, "switch", "-c", "unexpected")
        self.finish(t, "r", rc=1)
        self.assertTrue(w.is_dir())
        t = self.term("operation")
        w = self.worktrees / "operation"
        gitdir = Path(self.git(w, "rev-parse", "--absolute-git-dir"))
        (gitdir / "CHERRY_PICK_HEAD").write_text(
            self.git(w, "rev-parse", "HEAD") + "\n"
        )
        self.finish(t, "r", rc=1)
        self.assertTrue(w.is_dir())

    def test_detached_source_creates_branch_but_refuses_merge(self):
        self.git(self.repo, "checkout", "--detach")
        t = self.term("detached")
        w = self.worktrees / "detached"
        self.assertEqual(self.git(w, "branch", "--show-current"), "worktree/detached")
        self.finish(t, "m", rc=1)

    def test_another_tny_cannot_enter_an_active_worktree(self):
        t = self.term("busy")
        self.assertIn("in use", self.cli("--worktree=busy", "status", ok=False).stderr)
        self.finish(t)
        self.enter("busy")

    def test_existing_worktree_can_be_reentered_after_branch_switch(self):
        w = self.enter("switch-later")
        self.git(w, "switch", "-c", "later-branch")
        self.assertEqual(self.enter("switch-later"), w)
        self.assertEqual(self.git(w, "branch", "--show-current"), "later-branch")

    def test_slash_switches_workspace_and_starts_fresh_session(self):
        env = {**self.env, "TNY_ISOLATE": "1"}
        t = self.term(env=env)
        t.send("/worktree slash\r")
        t.expect("new session")
        t.send("/status\r")
        t.expect(str(self.worktrees / "slash"))
        t.send("/worktree second\r")
        t.expect("kept previous worktree")
        t.expect("/second")
        t.send("/worktree slash\r")
        t.expect_next("entered worktree")
        self.finish(t, "r")
        self.assertFalse((self.worktrees / "slash").exists())
        self.assertTrue((self.worktrees / "second").is_dir())

    def test_worktree_after_ssh_off_stays_local(self):
        helpers = self.root / "helpers"
        helpers.mkdir()
        log = self.root / "ssh-calls"
        fake = helpers / "ssh"
        fake.write_text(
            f"#!{Path(sys.executable).resolve()}\n"
            "import subprocess, sys\n"
            f"with open({str(log)!r}, 'a') as f: f.write('called\\n')\n"
            "if sys.argv[-1] == 'true' or 'exit' in sys.argv: sys.exit(0)\n"
            "sys.exit(subprocess.call(['sh', '-c', sys.argv[-1]]))\n"
        )
        fake.chmod(0o755)
        env = {**self.env, "PATH": str(helpers) + ":" + self.env["PATH"]}
        t = self.term(
            extra=("--ssh", "fixture.invalid", "--ssh-cwd", str(self.repo)), env=env
        )
        t.send("/ssh off\r")
        t.expect("disconnected from")
        before = log.read_text()
        t.send("/worktree local\r")
        t.expect("new session")
        self.assertEqual(log.read_text(), before)
        self.finish(t)

    def test_provider_tool_runs_in_worktree_after_slash(self):
        # Only the new checkout has this committed version of AGENTS.md.
        # The old workspace keeps an uncommitted instruction override.
        (self.repo / "AGENTS.md").write_text("SOURCE-ONLY-INSTRUCTIONS\n")
        port = free_port()
        mock_env = base_env(
            str(self.home),
            {
                "MOCK_CUSTOM_TOOL": "terminal",
                "MOCK_CUSTOM_ARGUMENTS": json.dumps({"command": "pwd > tool-cwd.txt"}),
                "MOCK_EXPECT_INSTRUCTIONS": "WORKTREE-INSTRUCTIONS",
                "MOCK_REJECT_INSTRUCTIONS": "SOURCE-ONLY-INSTRUCTIONS",
            },
        )
        mock = subprocess.Popen(
            [sys.executable, MOCK, str(port)],
            env=mock_env,
            stdout=subprocess.PIPE,
            text=True,
        )
        self.addCleanup(mock.wait)
        self.addCleanup(mock.terminate)
        self.assertIn("ready", mock.stdout.readline())
        self.addCleanup(mock.stdout.close)
        env = {
            **self.env,
            "TNY_ISOLATE": "1",
            "OPENAI_BASE_URL": f"http://127.0.0.1:{port}/v1",
            "OPENAI_API_KEY": "fixture",
        }
        t = self.term(env=env)
        t.send("/worktree tools\r")
        t.expect("new session")
        t.send("write cwd\r")
        t.expect("MOCK-OK")
        self.finish(t)
        w = self.worktrees / "tools"
        self.assertEqual((w / "tool-cwd.txt").read_text().strip(), str(w))
        self.assertFalse((self.repo / "tool-cwd.txt").exists())
        sessions = list((self.home / ".tny" / "sessions").glob("*/*/session.json"))
        self.assertTrue(
            any(json.loads(p.read_text())["workspace"] == str(w) for p in sessions)
        )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
