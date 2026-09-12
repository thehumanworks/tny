#!/usr/bin/env python3
"""Controlled #122 dimension/header and #126 image-input faults.

Same procedure as `open_issues_20260911.py` (issue-123 faults), same contract
(docs/verification/open-issues-2026-09-11). Every fault is applied to a
disposable copy of a frozen source tree, never to the checkout. A compiler
error, a timeout, an unrelated error or a test that did not run is never a
kill: only the mapped behavioral assertion failing counts.

    python3 tests/mutation/open_issues_20260911_image.py \
        --source <frozen source copy> --artifacts <new evidence directory>

`--plan-only` writes and validates the complete plan without building.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

DIMENSIONS = "src/core/image_dimensions.c"
SERVICE = "src/core/image_service.c"
CONFIG = "src/core/config.c"
RUNTIME = "src/core/runtime.c"
TOOLS = "src/core/tools.c"
CMD_ASK = "src/cli/cmd_ask.c"
# Every object below is deleted before each build so no earlier fault can
# survive in a stale object or in a binary that was not relinked.
TARGETS = (DIMENSIONS, SERVICE, CONFIG, RUNTIME, TOOLS, CMD_ASK)
BINARIES = ("build/tny-test", "build/tny")
WORKFLOW = "tests/integration/test_image_workflow.py"
IMAGE_INPUT = "tests/integration/test_image_input.py"

HEADERS_TEST = "image_dimensions_reject_impossible_headers"
POSITIVE_HEADERS_TEST = "image_dimensions_from_real_headers"
SIZE_TEST = "image_size_requests_and_status"
STRICT_UNIT_TEST = "image_strict_size_is_settled_before_any_request"
MAP_TEST = "image_input_map_resolves_per_provider_and_resets"
MALFORMED_TEST = "image_input_map_rejects_malformed_settings"
GATES_TEST = "image_input_false_gates_read_image_queue_and_flush"
SCHEMA_TEST = "image_input_false_gates_fully_configured_schema"
TURNS_TEST = "runtime_refuses_image_turns_when_image_input_is_configured_off"


def unit(test):
    return {"kind": "unit", "test": test}


def workflow(select):
    return {"kind": "integration", "script": WORKFLOW, "select": select}


def image_input(select):
    return {"kind": "integration", "script": IMAGE_INPUT, "select": select}


# id, file, intent, exact replacement, oracle, expected named assertion.
CASES = (
    {
        "id": "M122.1",
        "path": DIMENSIONS,
        "intent": "report a dimension mismatch as an exact match",
        "before": "    return want_width == width && want_height == height "
        "? TNY_IMAGE_SIZE_MATCH\n"
        "                                                        "
        ": TNY_IMAGE_SIZE_MISMATCH;",
        "after": "    return want_width == width && want_height == height "
        "? TNY_IMAGE_SIZE_MATCH\n"
        "                                                        "
        ": TNY_IMAGE_SIZE_MATCH;",
        "oracle": unit(SIZE_TEST),
        "assertion": 'ASSERT_EQ(TNY_IMAGE_SIZE_MISMATCH, tny_image_size_compare("2x3", '
        "TNY_IMAGE_DIM_OK, 2, 4))",
    },
    {
        "id": "M122.2",
        "path": SERVICE,
        "intent": "accept a strict-size mismatch instead of rejecting the paid bytes",
        "before": "    if (!r->strict_size || result->size_status == TNY_IMAGE_SIZE_MATCH) "
        "return 0;",
        "after": "    if (!r->strict_size || result->size_status == TNY_IMAGE_SIZE_MATCH ||\n"
        "        result->size_status == TNY_IMAGE_SIZE_MISMATCH)\n        return 0;",
        "oracle": workflow(
            "Dimensions.test_strict_size_rejects_before_replacing_the_destination"
        ),
        "assertion": "the destination keeps b'previous image bytes' and the run exits 1 "
        "with code IMAGE_SIZE_MISMATCH",
    },
    {
        "id": "M122.3",
        "path": SERVICE,
        "intent": "trust the requested size instead of the returned bytes",
        "before": "        tny_image_dim_status dimensions = tny_image_dimensions(\n"
        "            (const uint8_t *)image.data, image.len, &result->width, "
        "&result->height);",
        "after": "        tny_image_dim_status dimensions =\n"
        "            tny_image_size_parse(result->requested_size, &result->width, "
        "&result->height)\n"
        "                ? TNY_IMAGE_DIM_OK\n"
        "                : tny_image_dimensions((const uint8_t *)image.data, image.len,\n"
        "                                       &result->width, &result->height);",
        "oracle": workflow(
            "Dimensions.test_reported_dimensions_ignore_the_requested_size"
        ),
        "assertion": "(result['width'], result['height']) == (1935, 811) for a 3440x1440 "
        "request",
    },
    {
        "id": "M122.4",
        "path": DIMENSIONS,
        "intent": "report an omitted/auto request as a mismatch",
        "before": '    if (!requested || !*requested || strcmp(requested, "auto") == 0) '
        "return TNY_IMAGE_SIZE_AUTO;",
        "after": '    if (!requested || !*requested || strcmp(requested, "auto") == 0)\n'
        "        return TNY_IMAGE_SIZE_MISMATCH;",
        "oracle": unit(SIZE_TEST),
        "assertion": "ASSERT_EQ(TNY_IMAGE_SIZE_AUTO, tny_image_size_compare(NULL, "
        "TNY_IMAGE_DIM_OK, 1, 1))",
    },
    {
        "id": "M122.H1",
        "path": DIMENSIONS,
        # The checksum is still computed, but never compared with the stored
        # CRC: removing the call outright would only be an unused-function
        # compile error, which is not a kill.
        "intent": "drop the IHDR checksum guard (A6)",
        "before": "        d[28] > 1 || png_header_crc(d + 12) != be32(d + 29))",
        "after": "        d[28] > 1 || png_header_crc(d + 12) != png_header_crc(d + 12))",
        "oracle": unit(HEADERS_TEST),
        "assertion": "ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(corrupt, "
        "sizeof corrupt, &w, &h)) for the independently generated known_png",
    },
    {
        "id": "M122.H2",
        "path": DIMENSIONS,
        "intent": "drop the required RIFF odd-payload padding guard (A6)",
        "before": "        if ((size & 1) && (size == end - (i + 8) || payload[size] != 0))\n"
        "            return TNY_IMAGE_DIM_UNVERIFIABLE;\n",
        "after": "",
        "oracle": unit(HEADERS_TEST),
        "assertion": "the two odd-payload WebP cases (missing pad, nonzero pad) must stay "
        "TNY_IMAGE_DIM_UNVERIFIABLE",
    },
    {
        "id": "M122.H3",
        "path": DIMENSIONS,
        "intent": "accept an unknown VP8L stream version (A6)",
        "before": "            if (bits >> 29) return TNY_IMAGE_DIM_UNVERIFIABLE;\n",
        "after": "",
        "oracle": unit(HEADERS_TEST),
        "assertion": "webp[24] |= 0x20 must stay TNY_IMAGE_DIM_UNVERIFIABLE",
    },
    {
        "id": "M122.H4",
        "path": DIMENSIONS,
        "intent": "drop the JPEG sample-precision guard (A6)",
        "before": "            if ((precision != 8 && (marker == 0xC0 || precision != 12)) || "
        "!components ||",
        "after": "            if (precision == 0 || !components ||",
        "oracle": unit(HEADERS_TEST),
        "assertion": "baseline precision 12 and DCT precision 16 must stay "
        "TNY_IMAGE_DIM_UNVERIFIABLE",
    },
    {
        "id": "M122.H5",
        "path": DIMENSIONS,
        "intent": "re-enable arithmetic lossless SOF11 as a supported frame (A6)",
        "before": "    return marker == 0xC0 || marker == 0xC1 || marker == 0xC2 || "
        "marker == 0xC9 || marker == 0xCA;",
        "after": "    return marker == 0xC0 || marker == 0xC1 || marker == 0xC2 || "
        "marker == 0xC9 ||\n           marker == 0xCA || marker == 0xCB;",
        "oracle": unit(HEADERS_TEST),
        "assertion": "ASSERT_EQ(TNY_IMAGE_DIM_UNSUPPORTED, ...) for frame 0xCB",
    },
    {
        "id": "M122.H6",
        "path": DIMENSIONS,
        "intent": "drop the VP8X canvas pixel-product bound (A6)",
        "before": "            if ((uint64_t)width * height > UINT32_MAX) "
        "return TNY_IMAGE_DIM_UNVERIFIABLE;\n",
        "after": "",
        "oracle": unit(HEADERS_TEST),
        "assertion": "VP8X 65536x65536 must stay TNY_IMAGE_DIM_UNVERIFIABLE",
    },
    {
        "id": "M122.H7",
        "path": DIMENSIONS,
        "intent": "classify an incomplete final RIFF chunk header as unsupported (A8)",
        "before": "    return i == end ? TNY_IMAGE_DIM_UNSUPPORTED : TNY_IMAGE_DIM_UNVERIFIABLE;",
        "after": "    return TNY_IMAGE_DIM_UNSUPPORTED;",
        "oracle": unit(HEADERS_TEST),
        "assertion": "the two-byte partial trailing chunk must be "
        "TNY_IMAGE_DIM_UNVERIFIABLE, not TNY_IMAGE_DIM_UNSUPPORTED",
    },
    {
        "id": "M126.C1",
        "path": RUNTIME,
        "intent": "delete the tny_engine_start image gate",
        "before": "    if (images && images[0] && tny_image_input_refused(e->ctx)) {\n"
        '        if (err && errlen) snprintf(err, errlen, "%s", '
        "TNY_IMAGE_INPUT_REFUSAL);\n        return -1;\n    }\n",
        "after": "",
        "oracle": unit(TURNS_TEST),
        "assertion": "tny_engine_start must return -1 with TNY_IMAGE_INPUT_REFUSAL, send "
        "nothing and leave the session messages unchanged",
    },
    {
        "id": "M126.C2",
        "path": RUNTIME,
        "intent": "delete the tny_engine_queue_image refusal",
        "before": "    if (e && tny_image_input_refused(e->ctx)) {\n"
        '        if (err && errlen) snprintf(err, errlen, "%s", '
        "TNY_IMAGE_INPUT_REFUSAL);\n        return -1;\n    }\n",
        "after": "",
        "oracle": unit(TURNS_TEST),
        "assertion": "the queued error stays TNY_IMAGE_INPUT_REFUSAL instead of the "
        "transport message",
    },
    {
        "id": "M126.C3",
        "path": TOOLS,
        "intent": "delete the tools_queue_image direct-call refusal",
        "before": "    if (env && tny_image_input_refused(env->ctx)) {\n"
        '        if (err && errlen) snprintf(err, errlen, "%s", '
        "TNY_IMAGE_INPUT_REFUSAL);\n        return -1;\n    }\n",
        "after": "",
        "oracle": unit(GATES_TEST),
        "assertion": "tools_queue_image must return -1 and leave n_pending_images at 1",
    },
    {
        "id": "M126.C4",
        "path": TOOLS,
        "intent": "let tools_flush_images flush instead of refusing",
        "before": "    if (tny_image_input_refused(env->ctx)) {\n"
        '        if (err && errlen) snprintf(err, errlen, "%s", '
        "TNY_IMAGE_INPUT_REFUSAL);\n        return -1;\n    }\n",
        "after": "",
        "oracle": unit(GATES_TEST),
        "assertion": "the refused flush returns -1 and preserves the pending entry, count "
        "and session message count",
    },
    {
        "id": "M126.C5",
        "path": TOOLS,
        "intent": "drop the read_image clause from schema_tool_disabled",
        "before": '    if (strcmp(name, "read_image") == 0 && '
        "tny_image_input_refused(env->ctx)) return true;\n",
        "after": "",
        "oracle": unit(GATES_TEST),
        "assertion": "read_image must disappear from the schema and tools_execute must "
        'answer with an "error:" prefix',
    },
    {
        "id": "M126.C6",
        "path": CMD_ASK,
        "intent": "delete the cmd_ask pre-session image check",
        "before": "    if (n_images && tny_image_input_refused(ctx)) {\n"
        '        fprintf(stderr, "tny: %s\\n", TNY_IMAGE_INPUT_REFUSAL);\n'
        "        buf_free(&prompt);\n        return 1;\n    }\n",
        "after": "",
        "oracle": image_input(
            "ImageInputTests."
            "test_configured_false_refuses_cli_image_without_request_or_session"
        ),
        "assertion": "self.assertEqual(r.returncode, 1, r.stderr) — the plain-ask oracle "
        "does not create state files either way, so its exit-status assertion is what "
        "distinguishes the pre-session check",
        # The planned session-state observable is real, but it belongs to the
        # sibling resume oracle: recorded, not counted towards the kill.
        "also": [
            {
                "oracle": image_input(
                    "ImageInputTests."
                    "test_configured_false_refuses_resume_image_before_touching_the_session"
                ),
                "assertion": "self.assertEqual(self.state_files() - before_files, set()) "
                "— the refused resume must not create the session lock",
            }
        ],
    },
    {
        "id": "M126.C7",
        "path": CONFIG,
        "intent": "let unknown authorize an automatic preview",
        "before": "    return tny_image_input_configured(ctx) == "
        "TNY_IMAGE_INPUT_CONFIGURED_SUPPORTED;",
        "after": "    return tny_image_input_configured(ctx) != "
        "TNY_IMAGE_INPUT_CONFIGURED_UNSUPPORTED;",
        "oracle": unit(MAP_TEST),
        "assertion": "ASSERT_FALSE(tny_image_input_auto_preview_allowed(ctx)) for the "
        "unknown and configured-off providers",
    },
    {
        "id": "M126.C8",
        "path": CONFIG,
        "intent": "keep a stale capability across provider resolution",
        "before": "    ctx->image_input = TNY_IMAGE_INPUT_UNKNOWN; "
        "/* recomputed per resolution */\n",
        "after": "",
        "oracle": unit(MAP_TEST),
        "assertion": "switching to a provider with no entry must return to "
        "TNY_IMAGE_INPUT_UNKNOWN",
    },
    {
        "id": "M126.C9",
        "path": CONFIG,
        "intent": "accept non-boolean map values",
        "before": "        if (!yyjson_is_bool(value)) {\n"
        '            fprintf(stderr, "tny: settings.json image_input.%s must be true or '
        'false\\n", name);\n            return -1;\n        }\n',
        "after": "",
        "oracle": unit(MALFORMED_TEST),
        "assertion": 'tny_resolve_backend must return -1 for "yes", 1, null and {} values',
    },
    {
        "id": "M126.C10",
        "path": CONFIG,
        # `seen` counts the key itself, so `< 1` never fires: the duplicate is
        # accepted and the lookup silently becomes last-wins. Deleting the whole
        # block instead would only leave image_input_same_key unused, and an
        # unused-function compile error is not a kill.
        "intent": "drop duplicate-key detection (last-wins lookup)",
        "before": "        if (seen != 1) {",
        "after": "        if (seen < 1) {",
        "oracle": unit(MALFORMED_TEST),
        "assertion": '{"openai":true,"openai":false} must fail configuration with -1',
    },
    {
        "id": "M126.C11",
        "path": CONFIG,
        "intent": "validate only the selected key instead of the whole map",
        "before": "        if (!image_input_key_valid(name, yyjson_get_len(key))) {",
        "after": "        if (!image_input_key_valid(name, yyjson_get_len(key)) &&\n"
        "            (!name || strcmp(name, wanted) == 0)) {",
        "oracle": unit(MALFORMED_TEST),
        "assertion": 'the unselected invalid keys ("", "open ai", "acp:claude", "acp@", an '
        "embedded NUL and a 257-byte key) must still fail configuration",
    },
    {
        "id": "M126.C12",
        "path": CONFIG,
        "intent": "relabel configured-true as plain supported",
        "before": '    case TNY_IMAGE_INPUT_CONFIGURED_SUPPORTED: return "configured, '
        'unverified";',
        "after": '    case TNY_IMAGE_INPUT_CONFIGURED_SUPPORTED: return "supported";',
        "oracle": unit(MAP_TEST),
        "assertion": 'ASSERT_STR_EQ("configured, unverified", tny_image_input_label(ctx))',
    },
    {
        "id": "M126.C13",
        "path": CONFIG,
        "intent": "look up the raw provider selector instead of canonical acp@NAME",
        "before": "    const char *agent = acp_provider_name(provider);",
        "after": "    const char *agent = NULL;",
        "oracle": unit(MAP_TEST),
        "assertion": "the acp:claude selector must resolve the acp@claude entry and stay "
        "refused",
    },
    {
        "id": "M126.C14",
        "path": TOOLS,
        "intent": "gate image generation on conversation image input",
        "before": "        return env->ctx->library_mode || env->ctx->ssh_host ||\n"
        '               !tny_image_capabilities(env->ctx, strcmp(name, "image_edit") == 0, '
        "NULL);",
        "after": "        return env->ctx->library_mode || env->ctx->ssh_host ||\n"
        "               tny_image_input_refused(env->ctx) ||\n"
        '               !tny_image_capabilities(env->ctx, strcmp(name, "image_edit") == 0, '
        "NULL);",
        "oracle": unit(GATES_TEST),
        "assertion": 'ASSERT(tool_schema_has(&env, "image_generate")) and "image_edit" '
        "while image input is configured off",
    },
    {
        "id": "M126.C15",
        "path": CONFIG,
        "intent": "drop image_input from the reserved settings keys",
        "before": '                                           "image_input",\n',
        "after": "",
        "oracle": unit(MALFORMED_TEST),
        "assertion": 'tny_provider_write_profile(ctx, "image_input", ...) must return -1 '
        'with "reserved settings key"',
    },
    {
        "id": "M126.C16",
        "path": TOOLS,
        "intent": "remove the capability predicate from the all-features raw-schema guard",
        "before": "         !tny_image_capabilities(env->ctx, false, NULL) || "
        "tny_image_input_refused(env->ctx))) {",
        "after": "         !tny_image_capabilities(env->ctx, false, NULL))) {",
        "oracle": unit(SCHEMA_TEST),
        "assertion": 'ASSERTm("fully configured schema must hide the refused read_image '
        'tool", hidden)',
    },
    {
        "id": "M126.C17",
        "path": CONFIG,
        "intent": "remove the bounded image_input map size",
        "before": "    if (yyjson_obj_size(map) > TNY_IMAGE_INPUT_MAX_ENTRIES) {\n"
        '        fprintf(stderr, "tny: settings.json image_input has more than %u '
        'providers\\n",\n                (unsigned)TNY_IMAGE_INPUT_MAX_ENTRIES);\n'
        "        return -1;\n    }\n",
        "after": "",
        "oracle": unit(MALFORMED_TEST),
        "assertion": "a 1025-entry map must fail while the 1024-entry map resolves",
    },
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def manifest(root):
    result = {}
    for base, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(
            d for d in dirs if d not in {".git", "build", "__pycache__", "node_modules"}
        )
        for name in sorted(files):
            path = Path(base) / name
            rel = path.relative_to(root).as_posix()
            if rel.startswith("docs/verification/"):
                continue
            result[rel] = (
                {"symlink": os.readlink(path)}
                if path.is_symlink()
                else {
                    "sha256": sha(path.read_bytes()),
                    "mode": oct(path.stat().st_mode & 0o777),
                }
            )
    return result


def store(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


# An allowlist, not a denylist: a caller's ambient credentials must not reach
# a build or a fixture because their variable happened not to match a pattern.
# The fixtures bring their own fake credentials and local HTTP endpoints.
INHERITED = ("PATH", "TMPDIR", "TERM", "LOGNAME", "USER", "SHELL")


def clean_env(home):
    env = {key: os.environ[key] for key in INHERITED if key in os.environ}
    env["HOME"] = str(home)
    env["LANG"] = "C.UTF-8"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def command(root, artifacts, name, argv, env, timeout, extra=None):
    """Run one recorded command. The caller's `extra` fields are stored too."""
    log = artifacts / (name + ".log")
    started = datetime.now(timezone.utc).isoformat()
    t0 = time.monotonic()
    with log.open("x") as out:
        try:
            process = subprocess.run(
                argv,
                cwd=root,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=out,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
            code, timed_out = process.returncode, False
        except subprocess.TimeoutExpired:
            code, timed_out = None, True
    text = log.read_text(errors="replace")
    record = {
        "name": name,
        "started_utc": started,
        "cwd": str(root),
        "argv": [str(a) for a in argv],
        "exit_code": code,
        "timed_out": timed_out,
        "seconds": round(time.monotonic() - t0, 3),
        "log": log.name,
        "log_sha256": sha(log.read_bytes()),
        "sources": {t: sha((root / t).read_bytes()) for t in TARGETS},
        "binaries": {
            b: sha((root / b).read_bytes()) if (root / b).exists() else None
            for b in BINARIES
        },
    }
    record.update(extra(text) if extra else {})
    store(artifacts / (name + ".json"), record)
    return record, text


def unit_counts(text):
    selected = re.search(r"Total: (\d+) test", text)
    return {"selected_tests": int(selected.group(1)) if selected else 0}


def run_unit(root, artifacts, name, oracle, env):
    record, text = command(
        root,
        artifacts,
        name,
        ["./build/tny-test", "-t", oracle["test"], "-v"],
        env,
        120,
        unit_counts,
    )
    count = record["selected_tests"]
    passed = (
        record["exit_code"] == 0 and count == 1 and "Pass: 1, fail: 0, skip: 0." in text
    )
    killed = (
        record["exit_code"] not in (None, 0)
        and not record["timed_out"]
        and count == 1
        and "Pass: 0, fail: 1, skip: 0." in text
        and re.search(r"FAIL.*" + re.escape(oracle["test"]), text) is not None
    )
    return passed, killed, record


def integration_counts(text):
    ran = re.search(r"^Ran (\d+) tests? in ", text, re.M)
    failures = re.search(r"^FAILED \((.*)\)$", text, re.M)
    detail = failures.group(1) if failures else ""
    return {
        "selected_tests": int(ran.group(1)) if ran else 0,
        "failures": int((re.search(r"failures=(\d+)", detail) or ["", "0"])[1]),
        "errors": int((re.search(r"errors=(\d+)", detail) or ["", "0"])[1]),
    }


def run_integration(root, artifacts, name, oracle, env):
    argv = [
        "python3",
        oracle["script"],
        str(root / "build/tny"),
        oracle["select"],
    ]
    record, text = command(root, artifacts, name, argv, env, 300, integration_counts)
    count, failed, errors = (
        record["selected_tests"],
        record["failures"],
        record["errors"],
    )
    passed = record["exit_code"] == 0 and count == 1 and re.search(r"^OK", text, re.M)
    # An unhandled exception (errors=) can come from a broken fixture or a lost
    # local socket; only an assertion failure is accepted as a behavioral kill.
    killed = (
        record["exit_code"] not in (None, 0)
        and not record["timed_out"]
        and count == 1
        and failed == 1
        and errors == 0
    )
    return bool(passed), killed, record


def run_oracle(root, artifacts, name, oracle, env):
    if oracle["kind"] == "unit":
        return run_unit(root, artifacts, name, oracle, env)
    return run_integration(root, artifacts, name, oracle, env)


def oracle_name(oracle):
    if oracle["kind"] == "unit":
        return "unit:" + oracle["test"]
    return "%s:%s" % (oracle["script"], oracle["select"])


def objects(root):
    """Debug and release object paths of every faultable source."""
    return [
        root / "build" / flavour / Path(target).with_suffix(".o")
        for target in TARGETS
        for flavour in ("dbg", "rel")
    ]


def compiled_sources(text):
    """Sources this make run actually recompiled, read from its own log.

    Timestamps are never trusted as evidence: the caller requires the faulted
    file to appear here, so a make that silently reused a stale object (GNU
    make 3.81 has one-second mtime granularity) is not mistaken for a rebuild.
    """
    return {
        "compiled": sorted(
            set(re.findall(r"-c -o build/(?:dbg|rel)/\S+\.o (\S+\.c)\b", text))
        )
    }


def rebuild(root, artifacts, name, env):
    """Purge every mutable object and both executables, then rebuild and link.

    The object of each faultable source and both linked binaries are removed,
    so neither a stale object nor an un-relinked executable can carry an
    earlier fault into the next case.
    """
    for path in objects(root) + [root / b for b in BINARIES]:
        path.unlink(missing_ok=True)
    stale = [str(p) for p in objects(root) + [root / b for b in BINARIES] if p.exists()]
    record, _ = command(
        root, artifacts, name, ["make", "-j8", *BINARIES], env, 900, compiled_sources
    )
    ok = record["exit_code"] == 0 and not record["timed_out"] and not stale
    return ok, dict(record, stale_paths=stale)


def build_plan(source):
    plan, problems = [], []
    originals = {path: (source / path).read_text() for path in TARGETS}
    for case in CASES:
        text = originals[case["path"]]
        count = text.count(case["before"])
        if count != 1:
            problems.append("%s: %d matches in %s" % (case["id"], count, case["path"]))
        plan.append(
            {
                "id": case["id"],
                "path": case["path"],
                "intent": case["intent"],
                "before": case["before"],
                "after": case["after"],
                "replacement_count": count,
                "oracle": oracle_name(case["oracle"]),
                "oracle_spec": case["oracle"],
                "expected_assertion": case["assertion"],
                "supplementary_oracles": [
                    {
                        "oracle": oracle_name(extra["oracle"]),
                        "expected_assertion": extra["assertion"],
                    }
                    for extra in case.get("also", ())
                ],
                "path_sha256": sha(text.encode()),
            }
        )
    return plan, problems, originals


def toolchain(root, env):
    versions = {}
    for name, argv in (
        ("cc", ["cc", "--version"]),
        ("make", ["make", "--version"]),
        ("python3", ["python3", "--version"]),
        ("uname", ["uname", "-a"]),
    ):
        try:
            out = subprocess.run(
                argv, cwd=root, env=env, capture_output=True, timeout=60, text=True
            )
            versions[name] = out.stdout.strip().splitlines()[:2]
        except (OSError, subprocess.SubprocessError) as error:
            versions[name] = ["unavailable: %s" % error]
    return versions


def results_table(outcomes):
    lines = [
        "ID | file | intent | oracle | verdict | evidence",
        "--- | --- | --- | --- | --- | ---",
    ]
    for outcome in outcomes:
        lines.append(
            "%s | `%s` | %s | `%s` | **%s** | %s"
            % (
                outcome["id"],
                outcome["path"],
                outcome["intent"],
                outcome["oracle"],
                outcome["verdict"],
                outcome.get("test_record") or outcome.get("reason", ""),
            )
        )
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path, required=True, help="frozen reviewed source directory"
    )
    parser.add_argument(
        "--artifacts", type=Path, required=True, help="new evidence directory"
    )
    parser.add_argument(
        "--plan-only",
        action="store_true",
        help="validate and record the complete plan without building or faulting",
    )
    parser.add_argument("--only", help="comma separated fault IDs (all by default)")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    artifacts = args.artifacts.resolve()
    if artifacts.is_relative_to(source):
        parser.error("artifacts must not be inside the frozen input directory")
    artifacts.mkdir(parents=True, exist_ok=False)
    (artifacts / "runner.py").write_bytes(Path(__file__).read_bytes())

    before = manifest(source)
    plan, problems, originals = build_plan(source)
    selected = set(args.only.split(",")) if args.only else None
    scratch = Path(tempfile.mkdtemp(prefix="tny-image-controlled-faults-"))
    root, home = scratch / "source", scratch / "home"
    home.mkdir()
    shutil.copytree(
        source,
        root,
        symlinks=True,
        ignore=shutil.ignore_patterns(".git", "build", "__pycache__", "node_modules"),
    )
    env = clean_env(home)
    store(
        artifacts / "plan.json",
        {
            "source": str(source),
            "disposable": str(root),
            "home": str(home),
            "source_manifest": before,
            "source_manifest_sha256": sha(json.dumps(before, sort_keys=True).encode()),
            "runner_sha256": sha(Path(__file__).read_bytes()),
            "planned_cases": len(plan),
            "plan_problems": problems,
            "selected": sorted(selected) if selected else "all",
            "environment": {
                "keys": sorted(env),
                "HOME": env["HOME"],
                "cwd": str(root),
            },
            "toolchain": toolchain(root, env),
            "mutants": plan,
        },
    )
    (artifacts / "plan.md").write_text(
        "# Planned controlled faults (%d)\n\n" % len(plan)
        + "ID | file | intent | oracle | expected named assertion\n--- | --- | --- | --- | ---\n"
        + "".join(
            "%s | `%s` | %s | `%s` | %s\n"
            % (c["id"], c["path"], c["intent"], c["oracle"], c["expected_assertion"])
            for c in plan
        )
    )
    if args.plan_only:
        print(json.dumps({"planned_cases": len(plan), "problems": problems}, indent=2))
        return 1 if problems else 0

    oracles = []
    for case in CASES:
        for spec in [case["oracle"]] + [e["oracle"] for e in case.get("also", ())]:
            if spec not in oracles:
                oracles.append(spec)
    # The positive header corpus is a baseline oracle too: a guard removal must
    # not be "killed" by breaking the valid fixtures instead.
    positive = unit(POSITIVE_HEADERS_TEST)
    strict = unit(STRICT_UNIT_TEST)
    for extra in (positive, strict):
        if extra not in oracles:
            oracles.append(extra)

    original_ok = not problems
    baseline_build = None
    baseline = []
    if original_ok:
        original_ok, baseline_build = rebuild(root, artifacts, "baseline-build", env)
    if original_ok:
        for index, oracle in enumerate(oracles):
            passed, _, record = run_oracle(
                root, artifacts, "baseline-%02d" % index, oracle, env
            )
            baseline.append(
                {
                    "oracle": oracle_name(oracle),
                    "passed": passed,
                    "selected_tests": record["selected_tests"],
                    "record": record["name"] + ".json",
                }
            )
            original_ok = passed and record["selected_tests"] == 1 and original_ok

    outcomes = []
    try:
        for case in CASES:
            mid = case["id"]
            outcome = {
                "id": mid,
                "path": case["path"],
                "intent": case["intent"],
                "oracle": oracle_name(case["oracle"]),
                "expected_assertion": case["assertion"],
                "verdict": "UNRUN",
            }
            if selected and mid not in selected:
                outcome["reason"] = "not selected by --only"
            elif not original_ok:
                outcome["reason"] = (
                    "unmodified build/oracles or the exact fault mapping did not pass"
                )
            else:
                text = originals[case["path"]]
                (root / case["path"]).write_text(
                    text.replace(case["before"], case["after"], 1)
                )
                built, build_record = rebuild(root, artifacts, mid + "-build", env)
                outcome["build_record"] = build_record["name"] + ".json"
                outcome["recompiled"] = case["path"] in build_record["compiled"]
                if not built:
                    outcome["verdict"] = "INVALID_BUILD_OR_TIMEOUT"
                elif not outcome["recompiled"]:
                    # The faulted source was not rebuilt, so the binaries under
                    # test are not this mutant: never report a verdict for it.
                    outcome["verdict"] = "NOT_REBUILT"
                else:
                    passed, killed, record = run_oracle(
                        root, artifacts, mid + "-test", case["oracle"], env
                    )
                    outcome["verdict"] = (
                        "KILLED_BEHAVIOR"
                        if killed
                        else "SURVIVED"
                        if passed
                        else "UNRELATED_FAILURE_OR_TIMEOUT"
                    )
                    outcome["test_record"] = record["name"] + ".json"
                    outcome["binaries_identical_to_baseline"] = bool(
                        baseline_build
                        and record["binaries"] == baseline_build["binaries"]
                    )
                    # Supplementary oracles are recorded evidence for the same
                    # mutant; the verdict above already stands on its own.
                    for index, extra in enumerate(case.get("also", ())):
                        _, extra_killed, extra_record = run_oracle(
                            root,
                            artifacts,
                            "%s-also-%d" % (mid, index),
                            extra["oracle"],
                            env,
                        )
                        outcome.setdefault("also", []).append(
                            {
                                "oracle": oracle_name(extra["oracle"]),
                                "expected_assertion": extra["assertion"],
                                "failed_as_expected": extra_killed,
                                "record": extra_record["name"] + ".json",
                            }
                        )
            outcomes.append(outcome)
            (root / case["path"]).write_text(originals[case["path"]])
            print(json.dumps(outcome), flush=True)
    finally:
        for path, text in originals.items():
            (root / path).write_text(text)

    restored, _ = rebuild(root, artifacts, "restored-build", env)
    restored_oracles = []
    if restored:
        for index, oracle in enumerate(oracles):
            passed, _, record = run_oracle(
                root, artifacts, "restored-%02d" % index, oracle, env
            )
            restored_oracles.append(
                {
                    "oracle": oracle_name(oracle),
                    "passed": passed,
                    "record": record["name"] + ".json",
                }
            )
            restored = passed and restored
    restored_bytes = all((root / p).read_text() == t for p, t in originals.items())
    source_unchanged = manifest(source) == before
    ran = [o for o in outcomes if o["verdict"] != "UNRUN"]
    complete = (
        original_ok
        and restored
        and restored_bytes
        and source_unchanged
        and bool(ran)
        and all(o["verdict"] == "KILLED_BEHAVIOR" for o in ran)
    )
    result = {
        "scope": "#122 M122.1-4 + A6/A8 header guards H1-H7 and #126 M126.C1-C17 only; "
        "manifest, export, job and preview families remain planned and unrun",
        "planned_cases": len(CASES),
        "executed_cases": len(ran),
        "original_passed": original_ok,
        "baseline_oracles": baseline,
        "mutants": outcomes,
        "restored_build_and_oracles_passed": restored,
        "restored_oracles": restored_oracles,
        "restored_source_bytes": restored_bytes,
        "frozen_source_unchanged": source_unchanged,
        "verdict": "PASS_THIS_FAULT_SET" if complete else "INCOMPLETE",
        "disposable_source": str(root),
    }
    store(artifacts / "result.json", result)
    (artifacts / "results.md").write_text(
        "# Controlled critical faults — #122 dimensions and #126 image input\n\n"
        "Planned %d, executed %d. Verdict: %s\n\n"
        % (len(CASES), len(ran), result["verdict"])
        + results_table(outcomes)
    )
    print(json.dumps({k: v for k, v in result.items() if k != "mutants"}, indent=2))
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
