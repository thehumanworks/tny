#!/usr/bin/env python3
"""The model runs `tny edit` inside `terminal`; tny intercepts it in-process.

The mock provider asks for one `terminal` call whose command is the documented
here-doc edit form, then asserts on the NEXT request that the tool output is
the CLI's own contract (`exit: 0` plus the verb's stdout) rather than a shell
transcript. The file on disk proves the edit really happened, and the absence
of a `tny` child proves it never became a nested process (docs/adr/0063).
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")

FENCE = "*** SEARCH\\nold line\\n*** REPLACE\\nnew line\\n*** END\\n"


def start_mock(env):
    proc = subprocess.Popen(
        [sys.executable, MOCK, "0"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    line = proc.stdout.readline().decode()
    assert line.startswith("ready on "), (line, proc.stderr.read().decode())
    return proc, int(line.split()[-1])


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def run_case(command, expected_output, expected_body):
    mock_env = dict(
        os.environ,
        MOCK_EXPECT_WIRE="responses",
        MOCK_CUSTOM_TOOL="terminal",
        MOCK_CUSTOM_ARGUMENTS=json.dumps({"command": command}, separators=(",", ":")),
        MOCK_EXPECT_TOOL_OUTPUT=expected_output,
    )
    mock, port = start_mock(mock_env)
    try:
        with tempfile.TemporaryDirectory() as home:
            ws = os.path.join(home, "ws")
            os.makedirs(ws)
            notes = os.path.join(ws, "notes.txt")
            with open(notes, "w", encoding="utf-8") as f:
                f.write("alpha\nold line\nomega\n")
            # a `tny` that refuses to run and reports the nesting environment:
            # an intercepted verb must never reach it, a non-intercepted one
            # must, and the child must be told it is nested (ADR 0063)
            shim = os.path.join(home, "bin")
            os.makedirs(shim)
            with open(os.path.join(shim, "tny"), "w", encoding="utf-8") as f:
                f.write(
                    "#!/bin/sh\n"
                    'echo "NESTED-CHILD-RAN $TNY_NESTED $TNY_NESTED_MODE" >&2\n'
                    "exit 97\n"
                )
            os.chmod(os.path.join(shim, "tny"), 0o755)
            env = dict(
                os.environ,
                HOME=home,
                PATH=f"{shim}{os.pathsep}{os.environ.get('PATH', '')}",
                OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
                OPENAI_API_KEY="synthetic-openai-key",
            )
            result = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "edit the notes"],
                env=env,
                capture_output=True,
                timeout=60,
            )
            assert result.returncode == 0, (
                result.returncode,
                result.stdout.decode(errors="replace"),
                result.stderr.decode(errors="replace"),
            )
            parsed = json.loads(result.stdout)
            assert parsed["exit_code"] == 0, parsed
            assert [t["name"] for t in parsed["tool_calls"]] == ["terminal"], parsed
            assert parsed["tool_calls"][0]["status"] == "success", parsed
            with open(notes, encoding="utf-8") as f:
                assert f.read() == expected_body, command
    finally:
        stop(mock)


def main():
    # here-doc payload: intercepted, and the file really changes
    run_case(
        f"tny edit notes.txt <<'TNY_EDIT'\n{FENCE.replace(chr(92) + 'n', chr(10))}TNY_EDIT\n",
        "exit: 0\nedited notes.txt: replaced 1 occurrence\n",
        "alpha\nnew line\nomega\n",
    )
    # a shape the intercept does not understand stays a shell command: the
    # PATH shim runs as a real child, and the file is untouched
    run_case(
        "tny edit notes.txt second.txt",
        "exit code: 97\noutput:\nNESTED-CHILD-RAN 1 yolo\n",
        "alpha\nold line\nomega\n",
    )
    print("intercept integration: ok")


if __name__ == "__main__":
    main()
