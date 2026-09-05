#!/usr/bin/env python3
"""Assert final visible rows using tmux's real terminal emulator, not raw output.

tmux is test-only. Each case owns a private socket/server and temporary HOME;
no running user server is contacted. CI and Nix supply tmux; other hosts skip
explicitly when it is unavailable.
"""

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ZSH = shutil.which(os.environ.get("ZSH", "zsh"))
TMUX_BIN = shutil.which(os.environ.get("TNY_TEST_TMUX_BIN", "tmux"))
WIDGET = Path(os.environ.get("TNY_TEST_WIDGET", ROOT / "shell/tny.zsh")).resolve()
ANSWER = "First answer line.\nSecond answer line.\nFINAL OUTPUT MUST REMAIN VISIBLE."

FAKE = """#!{python}
import json, sys, time
from pathlib import Path
sys.stdin.read()
config = json.loads(Path("answer.json").read_text())
text = config["text"] + ("\\n" if config["newline"] else "")
for start in range(0, len(text), 7):
    sys.stdout.write(text[start:start+7])
    sys.stdout.flush()
    time.sleep(0.002)
Path("done").touch()
sys.exit(config["exit"])
"""


class Terminal:
    def __init__(self, directory, columns=40, prompt_lines=2, rows=24):
        self.directory = directory
        self.socket = directory / "tmux.sock"
        self.env = dict(
            os.environ,
            HOME=str(directory),
            ZDOTDIR=str(directory),
            TERM="xterm-256color",
        )
        self.env.pop("TMUX", None)
        fake = directory / "answer"
        fake.write_text(FAKE.format(python=sys.executable))
        fake.chmod(0o755)
        prompt = "host ~\n" * (prompt_lines - 1) + "> "
        (directory / ".zshrc").write_text(
            f"PROMPT={shlex.quote(prompt)}; RPROMPT=''\n"
            "bindkey -v\n"
            f"TNY_BIN={shlex.quote(str(fake))}\n"
            f"source {shlex.quote(str(WIDGET))}\n"
            # A builtin-only hook records completion without moving the cursor.
            "zle-line-pre-redraw() { [[ -f done ]] && : > redrawn; }\n"
            "zle -N zle-line-pre-redraw\n"
        )
        try:
            self.run(
                "-f",
                "/dev/null",
                "new-session",
                "-d",
                "-x",
                str(columns),
                "-y",
                str(rows),
                "-s",
                "proof",
                f"exec {shlex.quote(ZSH)} -d -i",
            )
            self.wait(lambda: "> " in self.screen(), "initial shell prompt")
        except Exception:
            self.close()
            raise

    def run(self, *args):
        return subprocess.check_output(
            [TMUX_BIN, "-S", str(self.socket), *args],
            env=self.env,
            cwd=self.directory,
            text=True,
            stderr=subprocess.PIPE,
            timeout=10,
        )

    def screen(self, joined=True):
        return self.run(
            "capture-pane", "-p", *(["-J"] if joined else []), "-t", "proof"
        )

    def wait(self, predicate, description):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.02)
        raise AssertionError(f"Timed out waiting for {description}:\n{self.screen()}")

    def ask(
        self, text=ANSWER, newline=True, exit_code=0, question="What model are you?"
    ):
        for name in ("done", "redrawn"):
            (self.directory / name).unlink(missing_ok=True)
        (self.directory / "answer.json").write_text(
            json.dumps({"text": text, "newline": newline, "exit": exit_code})
        )
        self.run("send-keys", "-t", "proof", "-l", question)
        self.run("send-keys", "-t", "proof", "C-x", "a")
        self.wait(lambda: (self.directory / "redrawn").exists(), "post-answer redraw")
        # A new edit forces another redraw. Seeing it proves ZLE has returned
        # to editing, and prevents a pre-redraw screen from satisfying the test.
        self.run("send-keys", "-t", "proof", "-l", "__EDIT_READY__")
        self.wait(
            lambda: "__EDIT_READY__" in self.screen(), "next edit to be displayed"
        )
        return self.screen()

    def close(self):
        subprocess.run(
            [TMUX_BIN, "-S", str(self.socket), "kill-server"],
            env=self.env,
            capture_output=True,
            timeout=10,
        )


@unittest.skipUnless(ZSH and TMUX_BIN and os.name == "posix", "Zsh and tmux required")
class QuickAskScreenTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tny-screen-")
        self.terminal = None

    def tearDown(self):
        if self.terminal:
            self.terminal.close()
        self.temp.cleanup()

    def start(self, **kwargs):
        self.terminal = Terminal(Path(self.temp.name).resolve(), **kwargs)
        return self.terminal

    def assert_answer_before_prompt(self, screen, answer=ANSWER):
        self.assertIn(answer, screen, screen)
        self.assertLess(screen.index(answer) + len(answer), screen.rindex("> "), screen)

    def test_two_line_prompt_preserves_final_answer_row(self):
        self.assert_answer_before_prompt(self.start().ask())

    def test_four_line_prompt_and_unterminated_answer(self):
        self.assert_answer_before_prompt(self.start(prompt_lines=4).ask(newline=False))

    def test_wrapped_answer_and_question_on_narrow_screen(self):
        terminal = self.start(columns=24)
        self.assert_answer_before_prompt(
            terminal.ask(question="Explain why the last line must stay on screen.")
        )

    def test_failed_ask_preserves_answer_and_retry_buffer(self):
        screen = self.start().ask(exit_code=2)
        self.assert_answer_before_prompt(screen)
        self.assertIn("What model are you?__EDIT_READY__", screen)

    def test_scrolling_output_retains_final_lines(self):
        text = "padding\n" * 30 + ANSWER
        self.assert_answer_before_prompt(self.start(rows=12).ask(text=text))

    def test_one_line_prompt_still_works(self):
        self.assert_answer_before_prompt(self.start(prompt_lines=1).ask())


if __name__ == "__main__":
    unittest.main(verbosity=2)
