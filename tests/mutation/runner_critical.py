#!/usr/bin/env python3
"""Phase-3 behavioral mutants, private source copies, no product rewrites."""

import hashlib
import json
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MUTANTS = [
    (
        "writer-before-save",
        "runner",
        "    session_save(r.session);",
        "    session_lock_release(r.session);\n    session_save(r.session);",
        "bye && WIFEXITED",
    ),
    (
        "close-transferred-descriptor",
        "runner",
        "    c->fd.adopt(connection.release());",
        "    c->fd.adopt(connection.borrow());",
        "send_result == 0",
    ),
    (
        "signal-metadata-pid",
        "jobs",
        '    if (!had_selection) jm_set_bool(t.doc, root, "cancel_requested", true);',
        '    tny_jobs_host_signal_owned((pid_t)jm_int(root, "pid", -1), SIGTERM);\n'
        '    if (!had_selection) jm_set_bool(t.doc, root, "cancel_requested", true);',
        "WIFEXITED(status)",
    ),
    (
        "unknown-is-complete",
        "jobs",
        '    if (yyjson_mut_equals_str(cleanup, "complete"))',
        '    if (yyjson_mut_equals_str(cleanup, "complete") || yyjson_mut_equals_str(cleanup, "unknown"))',
        "!cleanup_reclaimable(root)",
    ),
    (
        "replay-consumed-batch",
        "runner",
        "                       yyjson_mut_bool(r->session->doc, false));",
        "                       yyjson_mut_bool(r->session->doc, true));",
        "rn_disk_packet(session) == nullptr",
    ),
]


def execute(command, log, timeout=120):
    result = subprocess.run(
        command, cwd=ROOT, text=True, capture_output=True, timeout=timeout
    )
    log.write_text(result.stdout + result.stderr)
    return result


def main():
    directory = ROOT / "build/runner-mutations"
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(["make", "test-runner-ownership"], cwd=ROOT, check=True)
    values = subprocess.check_output(
        ["make", "-s", "-f", "Makefile", "-f", "-", "runner-mutant-vars"],
        cwd=ROOT,
        text=True,
        input="runner-mutant-vars:\n\t@printf '%s\\n' '$(CXX)' '$(call cppflags,$(DBG_CFLAGS))' "
        "'$(filter-out $(CXX_RUNTIME),$(DBG_LDFLAGS))' '$(RUNNER_OWNERSHIP_OBJS)'\n",
    ).splitlines()
    compiler, flags, linker, objects = map(shlex.split, values)
    results = []
    for name, module, old, new, reason in MUTANTS:
        source = ROOT / f"src/core/{module}.cpp"
        original = source.read_text()
        assert original.count(old) == 1, (name, "mutation anchor changed")
        fingerprint = hashlib.sha256(source.read_bytes()).hexdigest()
        mutant = directory / f"{name}.cpp"
        mutant.write_text(original.replace(old, new))
        binary = directory / name
        built = execute(
            [
                *compiler,
                *flags,
                f'-DTNY_{module.upper()}_SOURCE="{mutant}"',
                "-DTNY_ALLOC_TESTING=1",
                "tests/fixtures/runner_ownership.cpp",
                *objects,
                *linker,
                "-o",
                str(binary),
            ],
            directory / f"{name}-build.log",
        )
        assert built.returncode == 0, built.stdout + built.stderr
        with tempfile.TemporaryDirectory(prefix="tny-mutant-") as state:
            run = execute([str(binary), state], directory / f"{name}-test.log")
        killed = run.returncode != 0 and reason in run.stdout + run.stderr
        results.append(
            {
                "mutation": name,
                "exit": run.returncode,
                "killed": killed,
                "oracle": reason,
                "source_sha256": fingerprint,
            }
        )
        assert hashlib.sha256(source.read_bytes()).hexdigest() == fingerprint
        print(json.dumps(results[-1]), flush=True)
    (directory / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    subprocess.run(["make", "test-runner-ownership"], cwd=ROOT, check=True)
    assert all(result["killed"] for result in results), results


if __name__ == "__main__":
    main()
