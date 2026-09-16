#!/usr/bin/env python3
"""Behavioral phase-2 mutants; compile copies without modifying production."""

import hashlib
import json
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MUTANTS = [
    (
        "borrowed-payload",
        "src/core/owned_event.cpp",
        "owned->owned_bytes = total;",
        "owned->owned_bytes = total; owned->ev.text = event->text;",
        "runtime_all_payloads_survive_queue_transfer_and_teardown",
        "heap-use-after-free",
    ),
    (
        "wrong-generation",
        "src/lib/custom_tools.cpp",
        "call.generation != generation",
        "(call.generation != generation && false)",
        "runtime_async_leases_survive_all_invalidation_orders",
        "FAIL",
    ),
    (
        "early-async-release",
        "src/lib/custom_tools.cpp",
        "host->state = call;",
        "host->state = std::shared_ptr<call_state>(call.get(), [](call_state *) {}, tny::allocator<call_state>{});",
        "runtime_async_leases_survive_all_invalidation_orders",
        "heap-use-after-free",
    ),
    (
        "queue-byte-accounting",
        "src/core/runtime.c",
        "e->queue_bytes += copy->owned_bytes;",
        "(void)copy->owned_bytes;",
        "runtime_payload_byte_limit_and_accounting",
        "FAIL",
    ),
    (
        "second-terminal",
        "src/core/runtime.c",
        "append_owned(e, terminal);\n    e->finalize_pending = true;",
        "append_owned(e, terminal);\n    append_owned(e, event_copy(e, &terminal->ev));\n    e->finalize_pending = true;",
        "runtime_copies_events_and_suppresses_duplicate_terminal",
        "FAIL",
    ),
    (
        "allocating-settlement",
        "src/core/runtime.c",
        "static void prepare_reserved_event(tny_engine *e, tny_owned_event *o) {",
        "static void prepare_reserved_event(tny_engine *e, tny_owned_event *o) { free(tny_alloc_malloc(1));",
        "runtime_reserved_settlement_never_allocates",
        "FAIL",
    ),
]


def run(command, log):
    result = subprocess.run(
        command, cwd=ROOT, text=True, capture_output=True, timeout=120
    )
    log.write_text(result.stdout + result.stderr)
    return result


def main():
    directory = ROOT / "build/runtime-mutations"
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(["make", "test-runtime-ownership"], cwd=ROOT, check=True)
    variables = subprocess.check_output(
        ["make", "-s", "-f", "Makefile", "-f", "-", "mutant-vars"],
        cwd=ROOT,
        text=True,
        input="mutant-vars:\n\t@printf '%s\\n' '$(CC)' '$(CXX)' '$(DBG_CFLAGS)' "
        "'$(PARSER_TEST_CPPFLAGS)' '$(filter-out $(CXX_RUNTIME),$(DBG_LDFLAGS))' "
        "'$(sort $(RUNTIME_TEST_OBJS))'\n",
    ).splitlines()
    cc, cxx, cflags, cppflags, linker, objects = map(shlex.split, variables)
    results = []
    for name, source, old, new, test, reason in MUTANTS:
        path = ROOT / source
        original = path.read_text()
        assert original.count(old) == 1, (name, "mutation anchor changed")
        fingerprint = hashlib.sha256(path.read_bytes()).hexdigest()
        cpp = path.suffix == ".cpp"
        mutant = directory / (name + path.suffix)
        mutant.write_text(original.replace(old, new))
        obj = directory / (name + ".o")
        binary = directory / name
        flags = (
            cppflags
            if cpp
            else [
                *cflags,
                "-DTNY_ALLOC_TESTING=1",
                "-include",
                "src/util/alloc_override.h",
            ]
        )
        compiled = run(
            [*(cxx if cpp else cc), *flags, "-c", str(mutant), "-o", str(obj)],
            directory / (name + "-compile.log"),
        )
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        replaced = (
            "build/parser-test/" + source + ".o"
            if cpp
            else "build/runtime-test/" + source.removesuffix(".c") + ".o"
        )
        assert replaced in objects, replaced
        linked = run(
            [
                *cxx,
                *cppflags,
                "-o",
                str(binary),
                str(obj),
                *[p for p in objects if p != replaced],
                *linker,
            ],
            directory / (name + "-link.log"),
        )
        assert linked.returncode == 0, linked.stdout + linked.stderr
        checked = run(
            [str(binary), "-s", "runtime_suite", "-t", test],
            directory / (name + "-test.log"),
        )
        killed = checked.returncode != 0 and reason in checked.stdout + checked.stderr
        results.append(
            {
                "mutation": name,
                "exit": checked.returncode,
                "reason": reason,
                "killed": killed,
                "source_sha256": fingerprint,
            }
        )
        assert hashlib.sha256(path.read_bytes()).hexdigest() == fingerprint
        print(json.dumps(results[-1]), flush=True)
    (directory / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    subprocess.run(["make", "test-runtime-ownership"], cwd=ROOT, check=True)
    assert all(item["killed"] for item in results), results


if __name__ == "__main__":
    main()
