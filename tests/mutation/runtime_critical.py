#!/usr/bin/env python3
"""Behavioral phase-2 mutants; compile copies without modifying production."""

import hashlib
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MUTANTS = [
    (
        "lost-prefailed-callback-oom",
        "src/core/runtime.c",
        "    if (tny_alloc_scope_failed()) {\n        e->oom_pending = true;\n        return;\n    }",
        "    if (tny_alloc_scope_failed()) {\n        return;\n    }",
        "runtime_failed_scope_callback_oom_survives_scope_reset",
        "FAIL",
    ),
    (
        "lost-copy-failure-callback-oom",
        "src/core/runtime.c",
        "        if (tny_alloc_scope_failed()) e->oom_pending = true;\n"
        "        else e->overflow_pending = true;",
        "        if (tny_alloc_scope_failed()) e->oom_pending = false;\n"
        "        else e->overflow_pending = true;",
        "runtime_callback_oom_survives_allocator_scope_reset",
        "FAIL",
    ),
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
        "    terminal->hooks_done = true;\n    append_owned(e, terminal);",
        "    terminal->hooks_done = true;\n    append_owned(e, terminal);\n    append_owned(e, event_copy(e, &terminal->ev));",
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


def provider_settlement_mutation(directory):
    subprocess.run(["make", "lib-shared-fault"], cwd=ROOT, check=True)
    variables = subprocess.check_output(
        ["make", "-s", "-f", "Makefile", "-f", "-", "provider-mutant-vars"],
        cwd=ROOT,
        text=True,
        input="provider-mutant-vars:\n\t@printf '%s\\n' '$(CC)' '$(CXX)' "
        "'$(FAULT_PIC_CFLAGS)' '$(FAULT_PIC_OBJS)' '$(LIB_FAULT_LDFLAGS)' '$(LIB_FAULT_REAL)'\n",
    ).splitlines()
    cc, cxx, flags, objects, linker, library = map(shlex.split, variables)
    fixture = [
        sys.executable,
        "tests/integration/test_libtny_faults.py",
        "--reserved-only",
    ]
    baseline = run(
        [*fixture, library[0]], directory / "provider-settlement-baseline.log"
    )
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr
    source = ROOT / "src/backends/openai/openai.c"
    original = source.read_text()
    fingerprint = hashlib.sha256(source.read_bytes()).hexdigest()
    anchor = "if (tny_alloc_settling()) {"
    assert original.count(anchor) == 1
    mutant = directory / "provider-allocating-settlement.c"
    mutant.write_text(original.replace(anchor, anchor + " free(tny_alloc_malloc(1));"))
    obj = mutant.with_suffix(".o")
    compiled = run(
        [*cc, *flags, "-c", str(mutant), "-o", str(obj)],
        directory / "provider-settlement-compile.log",
    )
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    library_mutant = directory / Path(library[0]).name
    replaced = "build/fault-pic/src/backends/openai/openai.o"
    assert replaced in objects
    linked = run(
        [
            *cxx,
            "-o",
            str(library_mutant),
            str(obj),
            *[p for p in objects if p != replaced],
            *linker,
        ],
        directory / "provider-settlement-link.log",
    )
    assert linked.returncode == 0, linked.stdout + linked.stderr
    checked = run(
        [*fixture, str(library_mutant)], directory / "provider-settlement-test.log"
    )
    reason = "allocation during reserved OOM settlement"
    result = {
        "mutation": "provider-allocating-settlement",
        "exit": checked.returncode,
        "reason": reason,
        "killed": checked.returncode != 0 and reason in checked.stderr,
        "source_sha256": fingerprint,
    }
    assert hashlib.sha256(source.read_bytes()).hexdigest() == fingerprint
    print(json.dumps(result), flush=True)
    return result


def provider_failure_mutations(directory):
    """Challenge provider pre-settlement allocation boundaries and TERM escalation.

    Each mutant is compiled from a private copy and linked against the real
    allocator-instrumented object graph; the oracle is a behavioral assertion
    in the provider-fault host or the reserved-settlement Python fixture.
    """
    subprocess.run(
        ["make", "lib-shared-fault", "build/lib-fault/provider-faults"],
        cwd=ROOT,
        check=True,
    )
    variables = subprocess.check_output(
        ["make", "-s", "-f", "Makefile", "-f", "-", "failure-mutant-vars"],
        cwd=ROOT,
        text=True,
        input="failure-mutant-vars:\n\t@printf '%s\\n' '$(CC)' '$(CXX)' '$(FAULT_PIC_CFLAGS)' "
        "'$(FAULT_PIC_OBJS)' '$(LIB_FAULT_LDFLAGS)' '$(REL_LDFLAGS)' "
        "'$(PROVIDER_FAULT_TEST_OBJS)' '$(LIB_FAULT_REAL)'\n",
    ).splitlines()
    cc, cxx, flags, objects, library_link, host_link, test_objects, library = map(
        shlex.split, variables
    )
    cases = [
        (
            "parser-ordinary-finalization",
            "src/backends/openai/openai.c",
            "    oa_cancel(o->self);\n    tny_alloc_settlement_end();",
            "    session_save(o->env.session);\n    emit_turn_end(o, TNY_STOP_ERROR);\n"
            "    tny_alloc_settlement_end();",
            "request_construction_oom",
            "0 != tny_alloc_test_settlement_allocations()",
        ),
        (
            "decoder-observe-recovery",
            "src/backends/cursor/cursor.c",
            "if (rc == -2 || tny_alloc_scope_failed()) {",
            "if (rc == -2 || tny_alloc_scope_failed()) { free(tny_alloc_malloc(1));",
            "decoder_oom_mid_stream",
            "0 != tny_alloc_test_settlement_allocations()",
        ),
        (
            "acp-block-before-kill",
            "src/backends/acp/acp_client.c",
            "if (kill(-pgid, SIGKILL) != 0 && o->pid > 0) kill(o->pid, SIGKILL);",
            "/* mutant: omit escalation before blocking wait */",
            "emergency_cancel_reaps",
            "monotonic_ms() - start < 2000",
        ),
        (
            "request-oom-persists-usage",
            "src/backends/openai/openai.c",
            "request_oom:\n",
            "request_oom:\n    session_save(o->env.session);\n",
            "request_construction_oom",
            "0 != tny_alloc_test_settlement_allocations()",
        ),
        (
            "sdk-error-oom-fallback",
            "src/backends/cursor/sdk_error.c",
            "oom:\n    tny_alloc_provider_failed();",
            'oom:\n    free(xstrdup("fallback after decoder OOM"));\n'
            "    tny_alloc_provider_failed();",
            "error_decode_oom",
            "fault != tny_alloc_test_scope_count()",
        ),
        (
            "acp-parser-oom-next-line",
            "src/backends/acp/acp_proc.c",
            "oom:\n    tny_alloc_provider_failed();",
            'oom:\n    free(xstrdup("next buffered line after OOM"));\n'
            "    tny_alloc_provider_failed();",
            "message_oom",
            "fault != tny_alloc_test_scope_count()",
        ),
    ]
    results = []
    for name, source, old, new, test, reason in cases:
        path = ROOT / source
        original = path.read_text()
        assert original.count(old) == 1, name
        fingerprint = hashlib.sha256(path.read_bytes()).hexdigest()
        mutant = directory / (name + ".c")
        mutant.write_text(original.replace(old, new))
        obj = mutant.with_suffix(".o")
        compiled = run(
            [*cc, *flags, "-c", str(mutant), "-o", str(obj)],
            directory / (name + "-compile.log"),
        )
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        replaced = "build/fault-pic/" + source.removesuffix(".c") + ".o"
        assert replaced in objects
        binary = directory / (name if test else Path(library[0]).name)
        linked = run(
            [
                *cxx,
                "-o",
                str(binary),
                str(obj),
                *[p for p in objects if p != replaced],
                *(test_objects if test else []),
                *(host_link if test else library_link),
            ],
            directory / (name + "-link.log"),
        )
        assert linked.returncode == 0, linked.stdout + linked.stderr
        command = (
            [str(binary), "-t", test]
            if test
            else [
                sys.executable,
                "tests/integration/test_libtny_faults.py",
                "--reserved-only",
                str(binary),
            ]
        )
        if test:
            baseline = run(
                ["build/lib-fault/provider-faults", "-t", test],
                directory / (name + "-baseline.log"),
            )
            assert baseline.returncode == 0, baseline.stdout + baseline.stderr
        checked = run(command, directory / (name + "-test.log"))
        result = {
            "mutation": name,
            "exit": checked.returncode,
            "reason": reason,
            "killed": checked.returncode != 0
            and reason in checked.stdout + checked.stderr,
            "source_sha256": fingerprint,
        }
        assert hashlib.sha256(path.read_bytes()).hexdigest() == fingerprint
        results.append(result)
        print(json.dumps(result), flush=True)
    return results


def main():
    directory = ROOT / "build/runtime-mutations"
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(["make", "test-runtime-ownership"], cwd=ROOT, check=True)
    variables = subprocess.check_output(
        ["make", "-s", "-f", "Makefile", "-f", "-", "mutant-vars"],
        cwd=ROOT,
        text=True,
        input="mutant-vars:\n\t@printf '%s\\n' '$(CC)' '$(CXX)' '$(FAULT_SAN_PIC_CFLAGS)' "
        "'$(FAULT_SAN_PIC_CXXFLAGS)' '$(DBG_LDFLAGS)' "
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
            "build/fault-san-pic/" + source + ".o"
            if cpp
            else "build/fault-san-pic/" + source.removesuffix(".c") + ".o"
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
    results.append(provider_settlement_mutation(directory))
    results.extend(provider_failure_mutations(directory))
    (directory / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    subprocess.run(["make", "test-runtime-ownership"], cwd=ROOT, check=True)
    assert all(item["killed"] for item in results), results


if __name__ == "__main__":
    main()
