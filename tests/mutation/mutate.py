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
    ("src/net/stream.c", None, r"ossl|OSSL",
     "tests/integration/test_https.py"),
    ("src/tui/tui_prewarm.c", None, None),
    ("src/tui/tui_draw.c", ["tui_push_ansi", "tui_overlay_budget", "overlay_rows",
                            "tui_overlay_linef", "tui_overlay_clear"], None),
    ("src/tui/tui.c", ["ensure_backend", "drain_engine_events",
                       "tui_submit"], None),
    ("src/core/runtime.c", ["queue_event", "after_backend",
                            "tny_engine_next_event"],
     r"terminal|overflow|forcing|BACKPRESSURE|deadline|monotonic|poll",
     "tests/integration/test_libtny.py"),
    # extension control plane (#55): prompt/tool/permission precedence,
    # exact-once native boundaries, suppressed resolved permissions, and
    # terminal settlement after cancellation/deny.
    ("src/core/runtime.c",
     ["fold_extension_result", "process_queued_extension_hooks"],
     r"^(?!for ).*(permission|PERMISSION|suppressed|respond_permission|fold\.stop|fold\.blocked|deny|allow|selected)",
     "tests/integration/test_extensions.py"),
    ("src/core/runtime.c",
     ["native_pre_tool", "native_permission", "native_post_tool",
      "native_observe", "native_openai_control"],
     r"^(?!for ).*(stop|deny|rewrite|replacement|annotation|result_|audited|control_|cancel_probe|native_control|PERMISSION)",
     "tests/integration/test_extensions.py"),
    ("src/core/runtime.c", ["resolve_pending_terminal"],
     r"continue_requested|TNY_STOP_DENIED|TNY_STOP_INTERRUPTED|\bstop\b",
     "tests/integration/test_extensions.py"),
    ("src/core/session.c",
     ["session_replace_tool_arguments", "session_record_tool_audit"],
     None, "tests/integration/test_extensions.py"),
    ("src/core/tools.c", ["tools_call_prepare", "tools_call_execute",
                           "tools_execute"], None),
    ("src/tui/tui_input.c", ["do_key"], r"overlay"),
    ("src/core/config.c", ["tny_ctx_load"], r"perm_mode|permission_mode"),
    # named openai-compatible providers (settings.json profiles + env vars)
    ("src/core/config.c",
     ["tny_provider_name", "custom_provider_obj", "tny_provider_env_var",
      "derived_env_value", "tny_custom_provider_exists",
      "tny_custom_provider_key_env", "tny_env_provider_names",
      "env_sole_detected_provider", "load_openai_profile",
      "apply_custom_provider", "apply_provider_model", "tny_resolve_backend"],
     None),
    ("src/core/config.c", ["tny_effort_canonical", "tny_effort_wire",
                           "apply_provider_effort"], None),
    # mid-turn input: steer or queue (docs/adr/0011)
    ("src/tui/tui.c", ["tui_queue_push", "tui_queue_clear", "queue_pop",
                       "tui_cancel_turn", "after_turn", "tui_submit"],
     r"queue|steer"),
    ("src/backends/openai/openai.c", ["take_steer", "oa_steer", "step_finished",
                                      "emit_turn_end"],
     r"steer"),
    ("src/backends/openai/openai.c",
     ["run_tools", "finish_tool_batch", "oa_respond_permission", "oa_cancel"],
     None, "tests/integration/test_extensions.py"),
    ("src/backends/openai/openai.c",
     ["provider_control", "start_post_mode", "complete_tool", "execute_call",
      "finish_cancelled_call", "tool_batch_control", "subagent_control"],
     None, "tests/integration/test_extensions.py"),
    # streamed tool_call assembly: parallel calls, gateway index reuse
    ("src/backends/openai/toolcalls.c", None, None,
     "tests/integration/test_openai.py"),
    ("src/backends/codex/codex.c", ["cx_steer"], None),
    # --ssh remote tool runtime (docs/adr/0022): target parsing, the quoting
    # + stdin/timeout primitive, and every remote tool script
    ("src/core/ssh.c", ["ssh_target_set", "ssh_shell_quote", "ssh_run"], None),
    ("src/core/tools_ssh.c", None, None),
    # native grok device-code login / refresh / logout (docs/adr/0021)
    ("src/core/grok_login.c", None, None),
    # steer-text ownership + turn-end sweep (docs/adr/0013)
    ("src/backends/codex/codex_rpc.c", ["cx_end_turn", "cx_request"],
     r"steer|STEER|registered|CXR_FREE"),
    ("src/backends/codex/codex_msg.c", ["cx_response"], r"steer|STEER|CXR"),
    # --fast capability (TNY_CAP_FAST): new functions whole, only the
    # tier/fast lines inside the pre-existing ones.
    ("src/core/backend.c", ["tny_backend_caps"], None),
    # extension parity contract (#54): pure provider matrices, negotiated host
    # setup, typed unsupported diagnostics, and side-effect-free doctor JSON.
    ("src/core/extension_caps.c",
     ["tny_extension_capability_get", "tny_extension_capability_reason",
      "tny_extension_capabilities_json"], None,
     "tests/integration/test_extension_contract.py"),
    ("src/core/extensions.c", ["initialize_host", "append_action"],
     r"jget_int\(schema|schema_valid|selected_provider|unsupported_capability|unavailable_capability",
     "tests/integration/test_extension_capabilities.py"),
    ("src/cli/cmd_doctor.c", ["cmd_doctor"],
     r"NO_SPAWN|capabilit",
     "tests/integration/test_extension_capabilities.py"),
    ("src/core/config.c", ["tny_tier_is_fast"], None),
    ("src/cli/args.c", ["cli_parse_globals", "cli_make_ctx"],
     r"fast|tier|TNY_CAP"),
    ("src/tui/tui_commands.c", ["tui_command"], r"fast|tier"),
    ("src/backends/codex/codex.c", ["cx_start_thread"], r"tier|serviceTier",
     "tests/integration/test_codex.sh"),
    ("src/backends/openai/openai.c", ["build_request_chat"], r"tier",
     "tests/integration/test_openai.py"),
    ("src/backends/cursor/cursor.c",
     ["cursor_append_model_params", "append_options"], r"fast|tier",
     "tests/integration/test_cursor.sh"),
    # cursor SdkMessage envelope + tool_call union mapping (the opaque-tool
    # fix): the rewritten tool mapper whole, only the unwrap/result lines
    # inside the pre-existing handlers.
    ("src/backends/cursor/map.c",
     ["variant_tool_name", "tok_count", "emit_tool"], None,
     "tests/integration/test_cursor.sh"),
    ("src/backends/cursor/map.c", ["handle_sdk", "handle_result"],
     r"inner|itype|\brr\b|\bst\b|EXPIRED|ERROR",
     "tests/integration/test_cursor.sh"),
    # Responses API default wire (docs/adr/0016): the translation file and
    # the new backend functions whole, only the wire/stream_failed lines
    # inside the pre-existing ones.
    # provider setup (docs/adr/0018): key precedence and the profile writer
    # whole (unit-killed); the CLI flag parser and wizard against the
    # provider-setup / tui fixtures.
    ("src/core/config.c",
     ["tny_provider_write_profile", "edit_provider_profile"], None),
    ("src/core/config.c", ["apply_custom_provider", "load_openai_profile"],
     r"api_key"),
    ("src/cli/cmd_provider.c", ["provider_setup", "cmd_provider",
                                "base_url_ok"], None,
     "tests/integration/test_provider_setup.sh"),
    ("src/tui/tui_commands.c",
     ["tui_wizard_start", "tui_wizard_feed", "tui_wizard_cancel",
      "wiz_finish", "wiz_base_ok"], None),
    # wasm parity seams (docs/adr/0017): the poll wrapper, the relocated
    # URL parser, and the ACP ws transport (builders + routing + pump).
    ("src/util/tny_poll.c", None, None),
    ("src/net/url.c", None, None),
    ("src/backends/acp/acp_wire.c",
     ["acp_fmt_request", "acp_fmt_notify", "acp_fmt_result"], None,
     "tests/integration/test_acp.sh"),
    ("src/backends/acp/acp_proc.c",
     ["ac_agent_is_ws", "ac_tx_request", "ac_tx_notify", "ac_tx_result",
      "ac_tx_error", "push_fd", "ac_transport_pollfds", "ac_connect_ws"],
     None, "tests/integration/test_acp_ws.sh"),
    # ac_pump_reads: only the ws lines this change added; the stdio read
    # loop internals predate the transport seam and are latency-shaped.
    ("src/backends/acp/acp_proc.c", ["ac_pump_reads"], r"ws",
     "tests/integration/test_acp_ws.sh"),
    ("src/backends/openai/responses.c", None, None,
     "tests/integration/test_openai.py"),
    ("src/backends/openai/openai.c",
     ["build_request_rsp", "on_sse_event_rsp", "rsp_call_by_index",
      "on_sse_event"], r"^(?!.*reasoning_)",
     "tests/integration/test_openai.py"),
    # thinking deltas are dropped by `ask` (stderr noise); only the TUI
    # renders them, so these lines answer to the TUI suite
    ("src/backends/openai/openai.c", ["on_sse_event_rsp"], r"reasoning_"),
    ("src/backends/openai/openai.c", ["start_post", "oa_dispatch"],
     r"wire|stream_failed", "tests/integration/test_openai.py"),
    ("src/core/config.c",
     ["tny_wire_is_chat", "load_openai_profile", "apply_custom_provider"],
     r"wire", "tests/integration/test_openai.py"),
    ("src/cli/args.c", ["cli_parse_globals", "cli_make_ctx"], r"wire",
     "tests/integration/test_openai.py"),
    # builtin subscription profiles: claude/grok (docs/adr/0019). The login
    # ceremonies (codex_login.c, cmd_login) are interactive/process-spawning
    # and answer to tests/integration/test_codex.sh run 7 instead.
    ("src/core/profiles.c", None, None),
    ("src/core/config.c", ["tny_resolve_backend", "tny_provider_names_joined"],
     r"builtin|claude|grok"),
    ("src/backends/codex/codex_msg.c", ["cx_notification"], r"login",
     "tests/integration/test_codex.sh"),
    # color vs attribute split (docs/adr/0026): the resolver and the flag
    # parsing whole; the status row's two renderings answer to the unit
    # tests, the dumb-mode note + POLLNVAL exit to test_tui.py
    ("src/core/config.c", ["tny_color_resolve"], None),
    ("src/tui/tui_draw.c", ["tui_status_row"], None),
    ("src/cli/args.c", ["cli_parse_globals", "cli_make_ctx"], r"color"),
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
    # yyjson_arr_foreach is a zero-iteration no-op on NULL/non-arrays
    # (yyjson_arr_size returns 0), so the early-return guard is redundant
    # defense and flipping its ||/&& is unobservable.
    "toolcalls.c:if (!tool_calls || !yyjson_is_arr(tool_calls)) return;",
    # yyjson's read API is NULL/type-safe: yyjson_is_obj(NULL) is false,
    # yyjson_obj_foreach iterates zero times on non-objects and jget
    # (yyjson_obj_get) returns NULL for them, so flipping these &&/|| guards
    # is unobservable.
    "map.c:if (un && yyjson_is_obj(un)) {",
    "map.c:if (result && yyjson_is_obj(result)) {",
    # effort_from_settings is only read when reasoning_effort is non-NULL,
    # and every path that sets a value also sets the flag; the pre-recompute
    # reset is state hygiene for the value-not-found case (value NULL, flag
    # never consulted), so flipping it is unobservable.
    "config.c:ctx->effort_from_settings = false;",
    "tui_prewarm.c:pthread_cond_signal",  # signal-vs-broadcast style details
    # path_home() never returns NULL (it falls back to "/tmp"), so the
    # home_join NULL guards in the credential probes fire only on OOM.
    "profiles.c:if (!path) return false;",
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
    # Both fds are ttys under the pty fixture and neither is under pipes;
    # no harness we run can make exactly one a tty, so &&/|| here is
    # unobservable (the flag only gates optional prompts).
    "cmd_provider.c:    bool tty = isatty(0) && isatty(2);",
    # environ never holds two entries with the same key unless corrupted by
    # repeated putenv abuse; the dedupe branch is purely defensive.
    "config.c:if (strcmp(v[i], name) == 0) { dup = true; break; }",
    # Undersized allocations are only observable under ASan (the default
    # `make test` build); unsanitized glibc rounds tiny chunks up, so the
    # overwrite never faults here.
    "config.c:char *s = malloc(n + m + 1);",
    "config.c:char *name = malloc(plen + 1);",
    # -- Responses API wire (docs/adr/0016) --
    # `p && *p` -> `p || *p` on a possibly-NULL pointer: the dereference is
    # UB when p is NULL, and clang folds the release build back to the
    # original `p != NULL` check, so the mutant is equivalent by
    # optimization. (The && guards themselves are exercised: the mock sends
    # deltas with missing and empty payloads.)
    "openai.c:if (o->ctx->reasoning_effort && *o->ctx->reasoning_effort) {",
    "openai.c:if (d && *d) {",
    "openai.c:if (d && *d) emit_text(o, TNY_EV_THINKING, d, strlen(d));",
    # MAX_TOOL_CALLS boundary: observable only with a 17th parallel tool
    # call in one step; purely defensive against a hostile provider.
    "openai.c:if (!pc && o->ncalls < MAX_TOOL_CALLS) {",
    # "only set once" guards: flipping them re-strdups the same wire value
    # (a leak, no behavior change); ASan runs the unit suite, which cannot
    # drive the network handler.
    "openai.c:if (id && !pc->id) pc->id = xstrdup(id);",
    "openai.c:if (name && !pc->name) pc->name = xstrdup(name);",
    # loop-bound style flips guarded by the NULL-role/NULL-item skip on the
    # far side (arr_get past the end returns NULL), or arithmetically
    # identical at the boundary value (boundary>0 vs >=0 both start at 0).
    "responses.c:for (size_t i = boundary > 0 ? (size_t)boundary : 0; i < total; i++) {",
    # yyjson's foreach macros no-op on a non-container, so dropping the
    # is_arr/is_obj half of the guard cannot change the output (the
    # malformed-session unit test pins the contract either way).
    "responses.c:if (tcs && yyjson_mut_is_arr(tcs)) add_function_calls(d, arr, tcs);",
    "responses.c:if (js && yyjson_is_obj(js)) {",
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
