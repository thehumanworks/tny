#!/usr/bin/env python3
"""Targeted mutation testing for tny (stdlib only).

Generates single-token mutants inside a curated set of functions, rebuilds
the debug test binary, and checks that some test notices:

  1. `./build/tny-test`             (fast unit kill)
  2. `tests/integration/test_tui.py` (pty integration, only for survivors)

A mutant that compiles and passes BOTH stages is reported as SURVIVED and the
run exits nonzero: either the mutant is equivalent (annotate it below) or a
test is missing.

Usage:  python3 tests/mutation/mutate.py [--fast] [--only FILE_SUBSTR]
  --fast     skip the integration stage (report unit-survivors only)
  --only S   restrict to target files whose path contains S

Runtime is dominated by one `make debug` relink per mutant (~2-4 s each).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (file, [function names], line-must-match-regex)
# — functions None means the whole file; regex None means every line.
# The scope is the code THIS change touched: whole new files/functions, but
# only the overlay branches inside the pre-existing key handler.
# (path, function names or None, line regex or None[, integration test]) —
# the integration test kills survivors in full mode; default test_tui.py.
TARGETS = [
    # --fast capability (TNY_CAP_FAST): new functions whole, only the
    # tier/fast lines inside the pre-existing ones.
    ("src/core/backend.c", ["tny_backend_caps"], None),
    ("src/core/config.c", ["tny_tier_is_fast"], None),
    ("src/cli/args.c", ["cli_parse_globals", "cli_make_ctx"],
     r"fast|tier|TNY_CAP"),
    ("src/tui/tui_commands.c", ["tui_command"], r"fast|tier"),
    ("src/backends/codex/codex.c", ["cx_start_thread"], r"tier|serviceTier",
     "tests/integration/test_codex.sh"),
    ("src/backends/openai/openai.c", ["build_request"], r"tier",
     "tests/integration/test_openai.py"),
    ("src/backends/cursor/cursor.c",
     ["cursor_append_model_params", "append_options"], r"fast|tier",
     "tests/integration/test_cursor.sh"),
]

# operator substitutions applied to one site at a time
OPS = [
    (r"==", "!="), (r"!=", "=="),
    (r"<=", "<"), (r">=", ">"),
    (r"(?<![<>=!])<(?![<=])", "<="), (r"(?<![<>=!-])>(?![>=])", ">="),
    (r"&&", "||"), (r"\|\|", "&&"),
    (r"\btrue\b", "false"), (r"\bfalse\b", "true"),
    (r"\breturn -1\b", "return 0"), (r"\breturn 0\b", "return -1"),
    (r"\+ 1\b", "- 1"), (r"- 1\b", "+ 1"), (r"\+\+", "--"),
    (r"\bTNY_MODE_YOLO\b", "TNY_MODE_ASK"),
]

# Sites where a mutant is *equivalent* (no observable behavior change) or
# unobservable without heroics. Matched against "file:line-content".
EQUIVALENT = [
    "tui_prewarm.c:pthread_cond_signal",  # signal-vs-broadcast style details
    # Wrong poll direction only delays the retry by the poll timeout; the
    # handshake/write loops re-check the real condition and still succeed.
    "stream.c:struct pollfd pf = {fd, want == OSSL_ERROR_WANT_WRITE",
    # CA-bundle fallback order: every test host has the first bundle, so the
    # loop breaks before the increment ever runs; observing p-- needs a
    # machine with no standard CA bundle at all.
    "stream.c:for (const char *const *p = ossl_ca_bundles",
    # EOF classification: every caller treats clean EOF (0) and error (-1)
    # identically once a close-delimited body is complete, so flipping
    # ZERO_RETURN / SYSCALL-EOF between 0 and -1 is unobservable until a
    # caller starts detecting truncation.
    "stream.c:if (e == OSSL_ERROR_ZERO_RETURN) return 0;",
    "stream.c:if (e == OSSL_ERROR_SYSCALL) return n == 0 ? 0 : -1;",
    # A fatal mid-request write error and the "connection lost before
    # response" it causes downstream both fail the turn the same way, so
    # returning 0 here is indistinguishable without a proto-level probe.
    "stream.c:if (!ossl_want_retry(e)) return -1;",
]


def function_ranges(text, names):
    """Byte ranges of the named function bodies (brace matching)."""
    if names is None:
        return [(0, len(text))]
    spans = []
    for name in names:
        m = re.search(r"^[a-zA-Z_][^\n=;]*\b%s\(" % re.escape(name), text, re.M)
        if not m:
            continue
        i = text.find("{", m.end())
        if i < 0:
            continue
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        spans.append((m.start(), j + 1))
    return spans


def in_spans(spans, pos):
    return any(a <= pos < b for a, b in spans)


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def gen_mutants(path, names, line_re):
    text = open(path).read()
    spans = function_ranges(text, names)
    lines = text.split("\n")
    out = []
    for pat, repl in OPS:
        for m in re.finditer(pat, text):
            if not in_spans(spans, m.start()):
                continue
            ln = line_of(text, m.start())
            content = lines[ln - 1].strip()
            if line_re and not re.search(line_re, content):
                continue
            if content.startswith("/*") or content.startswith("*") or content.startswith("//"):
                continue
            # skip matches inside a trailing comment on a code line
            ls = m.start() - (len(text[:m.start()].split("\n")[-1]))
            before = text[ls:m.start()]
            if "//" in before or ("/*" in before and "*/" not in before.split("/*")[-1]):
                continue
            key = "%s:%s" % (os.path.basename(path), content)
            if any(eq in key for eq in EQUIVALENT):
                continue
            mutated = text[:m.start()] + re.sub(pat, repl.replace("\\", "\\\\"),
                                                m.group(0)) + text[m.end():]
            out.append({"file": path, "line": ln, "op": "%s -> %s" % (m.group(0), repl),
                        "content": content, "text": mutated})
    return out


def run(cmd, timeout, cwd=ROOT):
    try:
        r = subprocess.run(cmd, cwd=cwd, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return r.returncode, r.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -9, "(timeout after %ss)" % timeout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--only")
    args = ap.parse_args()

    # every integration fixture accepts the binary via $TNY (the .sh ones
    # default to per-backend build dirs the harness does not rebuild)
    os.environ["TNY"] = os.path.join(ROOT, "build", "tny")

    mutants = []
    for spec in TARGETS:
        path, names, line_re = spec[:3]
        itest = spec[3] if len(spec) > 3 else "tests/integration/test_tui.py"
        if args.only and args.only not in path:
            continue
        ms = gen_mutants(os.path.join(ROOT, path), names, line_re)
        for mu in ms:
            mu["itest"] = itest
        mutants += ms
    print("generated %d mutants" % len(mutants))

    killed_unit = killed_int = invalid = 0
    survivors = []
    t0 = time.time()
    for i, mu in enumerate(mutants):
        orig = open(mu["file"]).read()
        open(mu["file"], "w").write(mu["text"])
        # ancient GNU make (3.81, macOS) has 1-second mtime granularity: a
        # mutant written <1s after the previous restore would NOT rebuild
        # and the tests would run against the original code. Force it.
        now = time.time()
        os.utime(mu["file"], (now + 2, now + 2))
        tag = "%s:%d [%s]" % (os.path.relpath(mu["file"], ROOT), mu["line"], mu["op"])
        try:
            rc, out = run(["make", "debug"], 180)
            if rc != 0:
                invalid += 1
                print("%3d/%d  invalid   %s" % (i + 1, len(mutants), tag))
                continue
            rc, out = run(["./build/tny-test"], 60)
            if rc != 0:
                killed_unit += 1
                print("%3d/%d  killed:u  %s" % (i + 1, len(mutants), tag))
                continue
            if args.fast:
                survivors.append(mu)
                print("%3d/%d  SURVIVED(unit)  %s" % (i + 1, len(mutants), tag))
                continue
            rc, out = run(["make", "release"], 180)  # integration drives build/tny
            if rc != 0:
                invalid += 1
                print("%3d/%d  invalid:r %s" % (i + 1, len(mutants), tag))
                continue
            if mu["itest"].endswith(".sh"):  # bash fixtures take TNY from env
                rc, out = run(["bash", mu["itest"], "./build/tny"], 420)
            else:
                rc, out = run([sys.executable, mu["itest"], "./build/tny"], 420)
            if rc != 0:
                killed_int += 1
                print("%3d/%d  killed:i  %s" % (i + 1, len(mutants), tag))
            else:
                survivors.append(mu)
                print("%3d/%d  SURVIVED  %s" % (i + 1, len(mutants), tag))
        finally:
            open(mu["file"], "w").write(orig)
            now = time.time()  # same granularity trap on the restore
            os.utime(mu["file"], (now + 2, now + 2))
    # restore builds to pristine state
    run(["make", "debug"], 300)
    run(["make", "release"], 300)

    total = len(mutants) - invalid
    print("\n== mutation results (%.0fs) ==" % (time.time() - t0))
    print("valid mutants : %d (invalid/uncompilable: %d)" % (total, invalid))
    print("killed by unit: %d" % killed_unit)
    print("killed by int : %d" % killed_int)
    print("survived      : %d" % len(survivors))
    for mu in survivors:
        print("  %s:%d  %s   | %s" % (os.path.relpath(mu["file"], ROOT),
                                      mu["line"], mu["op"], mu["content"]))
    if total:
        print("kill ratio    : %.1f%%" % (100.0 * (killed_unit + killed_int) / total))
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
