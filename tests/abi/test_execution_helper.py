#!/usr/bin/env python3
"""Exercise the public helper ABI and fail-closed pre-snapshot handshake."""

from __future__ import annotations

import json
import os
import platform
import selectors
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r"""
#define _POSIX_C_SOURCE 200809L
#include "tny/tny.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static tny_bytes bytes(const char *s) {
    tny_bytes b = {s, s ? strlen(s) : 0};
    return b;
}

static int helper(void) {
    const char *mode = getenv("TNY_TEST_HELPER_MODE");
    if (mode && strcmp(mode, "wrong-version") == 0) {
        const char *identity = "{\"tny_sdk_executor\":1,\"version\":\"wrong\"}";
        size_t n = strlen(identity);
        unsigned char header[4] = {0, 0, 0, (unsigned char)n};
        assert(write(3, header, sizeof header) == (ssize_t)sizeof header);
        assert(write(3, identity, n) == (ssize_t)n);
    } else {
        /* EOF during identity, while keeping the receive half open to detect
         * any illicit credential/context transfer before admission. */
        assert(shutdown(3, SHUT_WR) == 0);
    }
    unsigned char buffer[1024];
    size_t received = 0;
    ssize_t n;
    while ((n = read(3, buffer, sizeof buffer)) > 0) received += (size_t)n;
    const char *log = getenv("TNY_TEST_HELPER_LOG");
    assert(log);
    FILE *f = fopen(log, "w");
    assert(f);
    fprintf(f, "%zu\n", received);
    assert(fclose(f) == 0);
    return 0;
}

static void status_is(int32_t actual, int32_t expected, tny_error **error) {
    if (actual != expected) {
        tny_bytes message = tny_error_message(*error);
        fprintf(stderr, "expected %d, got %d: %.*s\n", expected, actual,
                (int)message.len, message.ptr ? message.ptr : "");
        abort();
    }
    tny_error_free(*error);
    *error = NULL;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--exec-server") == 0) return helper();
    assert(argc == 5);
    const char *mode = argv[1], *workspace = argv[2], *path = argv[3], *url = argv[4];
    tny_runtime_options_v0 options;
    assert(tny_runtime_options_init(&options, sizeof options) == TNY_STATUS_OK);
    options.workspace = bytes(workspace);
    options.base_url = bytes(url);
    options.api_key = bytes("synthetic-must-not-reach-unmatched-helper");
    options.permission_mode = TNY_PERMISSION_YOLO;
    options.max_steps = 2;
    tny_runtime *runtime = NULL;
    tny_error *error = NULL;
    status_is(tny_runtime_create(&options, sizeof options, &runtime, &error), TNY_STATUS_OK, &error);
    const char *invalid[] = {"", "relative-executor", "/not-present-tny-helper", workspace};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        status_is(tny_runtime_set_execution_helper(runtime, bytes(invalid[i]), &error),
                  TNY_STATUS_INVALID_ARGUMENT, &error);
    char invalid_nul[] = {'/', 'x', 0, 'y'};
    tny_bytes invalid_view = {invalid_nul, sizeof invalid_nul};
    status_is(tny_runtime_set_execution_helper(runtime, invalid_view, &error),
              TNY_STATUS_INVALID_ARGUMENT, &error);
    status_is(tny_runtime_set_execution_helper(runtime, bytes(path), &error), TNY_STATUS_OK, &error);
    if (strcmp(mode, "removed") == 0) assert(unlink(path) == 0);
    tny_session *session = NULL;
    status_is(tny_session_create(runtime, &session, &error), TNY_STATUS_OK, &error);
    status_is(tny_runtime_set_execution_helper(runtime, bytes(path), &error), TNY_STATUS_BAD_STATE, &error);
    status_is(tny_session_send(session, bytes("Use the fixture tool once"), &error), TNY_STATUS_OK, &error);
    int ends = 0, tools = 0;
    for (int attempt = 0; attempt < 200 && !ends; attempt++) {
        tny_event *event = NULL;
        int32_t status = tny_session_next_event(session, 100, &event, &error);
        if (status == TNY_STATUS_TIMEOUT) continue;
        status_is(status, TNY_STATUS_EVENT, &error);
        uint32_t kind = tny_event_get_kind(event);
        if (kind == 3) {
            tools++;
            tny_bytes detail = tny_event_tool_detail(event);
            assert(detail.ptr && strstr(detail.ptr, "error:"));
        }
        if (kind == 7) ends++;
        tny_event_free(event);
    }
    assert(ends == 1 && tools == 1);
    status_is(tny_session_destroy(&session), TNY_STATUS_OK, &error);
    status_is(tny_runtime_destroy(&runtime), TNY_STATUS_OK, &error);
    return 0;
}
"""


def main() -> None:
    library = Path(
        os.environ.get(
            "TNY_TEST_LIBRARY",
            str(
                ROOT
                / "build/lib"
                / ("libtny.1.dylib" if platform.system() == "Darwin" else "libtny.so.1")
            ),
        )
    ).resolve()
    with tempfile.TemporaryDirectory(prefix="tny-helper-abi-") as temporary:
        root = Path(temporary)
        source = root / "consumer.c"
        source.write_text(SOURCE)
        consumer = root / "consumer"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include"),
                str(source),
                str(library),
                f"-Wl,-rpath,{library.parent}",
                "-o",
                str(consumer),
            ],
            check=True,
        )
        for mode in ("wrong-version", "eof-before-identity", "removed"):
            workspace = root / mode
            workspace.mkdir()
            executable = workspace / "helper"
            shutil.copy2(consumer, executable)
            log = workspace / "received"
            environment = {
                **os.environ,
                "MOCK_CUSTOM_TOOL": "run_code",
                "MOCK_CUSTOM_ARGUMENTS": json.dumps(
                    {"code": "print('must not execute')"}
                ),
                "TNY_TEST_HELPER_MODE": mode,
                "TNY_TEST_HELPER_LOG": str(log),
            }
            with subprocess.Popen(
                [
                    os.environ.get("PYTHON", "python3"),
                    str(ROOT / "tests/integration/mock_openai.py"),
                    "0",
                ],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            ) as mock:
                try:
                    assert mock.stdout
                    with selectors.DefaultSelector() as selector:
                        selector.register(mock.stdout, selectors.EVENT_READ)
                        assert selector.select(10), "mock readiness timeout"
                    ready = mock.stdout.readline().strip()
                    assert ready.startswith("ready on "), ready
                    url = f"http://127.0.0.1:{int(ready.split()[-1])}/v1"
                    subprocess.run(
                        [str(consumer), mode, str(workspace), str(executable), url],
                        env=environment,
                        check=True,
                        timeout=30,
                    )
                    if mode == "removed":
                        assert not log.exists(), "removed helper unexpectedly launched"
                    else:
                        assert log.read_text().strip() == "0", (
                            "context sent before identity admission"
                        )
                    print(f"helper ABI {mode}: passed", flush=True)
                finally:
                    mock.terminate()
                    try:
                        mock.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        mock.kill()
                        mock.wait()


if __name__ == "__main__":
    main()
