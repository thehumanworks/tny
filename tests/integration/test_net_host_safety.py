#!/usr/bin/env python3
"""Embedding-host regressions for native HTTP transport safety.

Build a deliberately tiny host around the production native transport.  The
host leaves SIGPIPE at its default disposition so a regression terminates the
child process, exactly as it would a Python or Node embedding process.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r"""
#include "net/net.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
	#include <arpa/inet.h>
	#include <netinet/in.h>
#include <sys/socket.h>
	#include <sys/wait.h>
#include <unistd.h>

static int closed_http_peer(void) {
    struct sigaction before = {0}, after = {0};
    before.sa_handler = SIG_DFL;
    sigemptyset(&before.sa_mask);
    if (sigaction(SIGPIPE, &before, NULL) != 0) return 10;

    sigset_t mask_before, mask_after;
    if (pthread_sigmask(SIG_SETMASK, NULL, &mask_before) != 0) return 11;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 12;
    close(sv[1]);
    http_conn *http = http_from_fd(sv[0]);
    if (!http) return 13;
    const char *headers[] = {"Authorization: Bearer regression-secret", NULL};
    int rc = http_request(http, "POST", "/v1/responses", headers, "{}", 2);
    http_close(http);

    if (sigaction(SIGPIPE, NULL, &after) != 0) return 14;
    if (pthread_sigmask(SIG_SETMASK, NULL, &mask_after) != 0) return 15;
    if (rc != -1) return 16;
    if (after.sa_handler != SIG_DFL) return 17;
    if (sigismember(&mask_before, SIGPIPE) !=
        sigismember(&mask_after, SIGPIPE)) return 18;
    return 0;
}

static int nonreading_peer(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 20;
    int small = 4096;
    if (setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small) != 0)
        return 21;
    if (set_nonblock(sv[0], true) != 0) return 22;
    nstream *stream = nstream_from_fd(sv[0]);
    if (!stream) return 23;

    size_t size = 16u * 1024u * 1024u;
    char *payload = malloc(size);
    if (!payload) return 24;
    memset(payload, 'x', size);
    int64_t started = monotonic_ms();
    int rc = nstream_write_all(stream, payload, size);
    int64_t elapsed = monotonic_ms() - started;
    free(payload);
    nstream_close(stream);
    close(sv[1]);

    if (rc != -1) return 25;
    /* Allow scheduler noise, but reject both a per-retry timeout reset and an
     * accidental immediate failure that never exercised backpressure. */
    if (elapsed < 4500 || elapsed > 8000) {
        fprintf(stderr, "write deadline elapsed=%lldms\n", (long long)elapsed);
        return 26;
    }
    return 0;
}

	static int closed_tls_peer(void) {
	    struct sigaction action = {0}, after = {0};
	    action.sa_handler = SIG_DFL;
	    sigemptyset(&action.sa_mask);
	    if (sigaction(SIGPIPE, &action, NULL) != 0) return 30;
	    sigset_t mask_before, mask_after;
	    if (pthread_sigmask(SIG_SETMASK, NULL, &mask_before) != 0) return 31;

	    /* Repeat to cover both first-use TLS initialization and the hot path. */
	    for (int round = 0; round < 4; round++) {
	        int listener = socket(AF_INET, SOCK_STREAM, 0);
	        if (listener < 0) return 32;
	        struct sockaddr_in address = {0};
	        address.sin_family = AF_INET;
	        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	        if (bind(listener, (struct sockaddr *)&address, sizeof address) != 0 ||
	            listen(listener, 1) != 0) return 33;
	        socklen_t address_len = sizeof address;
	        if (getsockname(listener, (struct sockaddr *)&address,
	                        &address_len) != 0) return 34;

	        pid_t child = fork();
	        if (child < 0) return 35;
	        if (child == 0) {
	            int peer = accept(listener, NULL, NULL);
	            if (peer >= 0) {
	                struct linger reset = {1, 0};
	                (void)setsockopt(peer, SOL_SOCKET, SO_LINGER,
	                                 &reset, sizeof reset);
	                close(peer);
	            }
	            close(listener);
	            _exit(peer >= 0 ? 0 : 1);
	        }

	        char error[256] = {0};
	        nstream *stream = nstream_connect(
	            "127.0.0.1", (int)ntohs(address.sin_port), true,
	            2000, error, sizeof error);
	        if (stream) nstream_close(stream);
	        int status = 0;
	        if (waitpid(child, &status, 0) != child) return 36;
	        close(listener);
	        if (stream != NULL || error[0] == '\0' || !WIFEXITED(status) ||
	            WEXITSTATUS(status) != 0) return 37;
	    }

	    if (sigaction(SIGPIPE, NULL, &after) != 0) return 38;
	    if (pthread_sigmask(SIG_SETMASK, NULL, &mask_after) != 0) return 39;
	    if (after.sa_handler != SIG_DFL ||
	        sigismember(&mask_before, SIGPIPE) !=
	        sigismember(&mask_after, SIGPIPE)) return 40;
	    return 0;
	}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
	    sigset_t pipe_signal;
	    sigemptyset(&pipe_signal);
	    sigaddset(&pipe_signal, SIGPIPE);
	    if (pthread_sigmask(SIG_UNBLOCK, &pipe_signal, NULL) != 0) return 4;
    if (strcmp(argv[1], "closed") == 0) return closed_http_peer();
    if (strcmp(argv[1], "deadline") == 0) return nonreading_peer();
	    if (strcmp(argv[1], "tls-closed") == 0) return closed_tls_peer();
    return 3;
}
"""


def compile_harness(tmp: Path) -> Path:
    source = tmp / "net_host_safety.c"
    binary = tmp / "net_host_safety"
    source.write_text(HARNESS)
    cmd = [
        os.environ.get("CC", "cc"),
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-deprecated-declarations",
        "-pthread",
        "-D_DARWIN_C_SOURCE",
        "-D_DEFAULT_SOURCE",
        "-D_BSD_SOURCE",
        "-Iinclude",
        "-Isrc",
        "-Ithird_party/picohttpparser",
        str(source),
        "src/net/tcp.c",
        "src/net/stream.c",
        "src/net/http1.c",
        "src/net/url.c",
        "src/util/util.c",
        "src/util/tny_poll.c",
        "third_party/picohttpparser/picohttpparser.c",
        "-ldl",
        "-o",
        str(binary),
    ]
    if sys.platform == "darwin":
        cmd.remove("-ldl")
    compiled = subprocess.run(cmd, cwd=ROOT, capture_output=True)
    assert compiled.returncode == 0, compiled.stderr.decode(errors="replace")
    return binary


def assert_secret_buffers_are_wiped() -> None:
    # These are the only native request builders that place Authorization in
    # an owned request buffer.  Keep the check close to buf_free so a future
    # early-exit refactor cannot silently bypass the wipe.
    for relative in ("src/net/http1.c", "src/net/ws.c"):
        source = (ROOT / relative).read_text()
        pattern = re.compile(
            r"nstream_write_all\([^;]+;\s*"
            r"if \(req\.data\) secure_zero\(req\.data, req\.cap\);\s*"
            r"buf_free\(&req\);",
            re.DOTALL,
        )
        assert pattern.search(source), f"request buffer is not wiped in {relative}"


def assert_tls_close_is_guarded() -> None:
    source = (ROOT / "src/net/stream.c").read_text()
    assert re.search(
        r"st_write\([^}]+sigpipe_guard_begin\(&guard\);"
        r".+write\(.+sigpipe_guard_end\(&guard\);",
        source,
        re.DOTALL,
    ), "SecureTransport callback writes are not guarded"
    assert re.search(
        r"sigpipe_guard_begin\(&guard\);\s*"
        r"rc = st_api\.handshake\(.+?sigpipe_guard_end\(&guard\);",
        source,
        re.DOTALL,
    ), "SecureTransport handshake is not guarded"
    assert re.search(
        r"sigpipe_guard_begin\(&guard\);\s*"
        r"st_api\.close\(.+?sigpipe_guard_end\(&guard\);",
        source,
        re.DOTALL,
    ), "SecureTransport close is not guarded"
    assert re.search(
        r"sigpipe_guard_begin\(&guard\);\s*"
        r"ossl\.shutdown\(.+?sigpipe_guard_end\(&guard\);",
        source,
        re.DOTALL,
    ), "OpenSSL close_notify is not guarded"


def main() -> None:
    assert_secret_buffers_are_wiped()
    assert_tls_close_is_guarded()
    with tempfile.TemporaryDirectory(prefix="tny-net-host-safety-") as raw:
        harness = compile_harness(Path(raw))
        closed = subprocess.run([harness, "closed"], capture_output=True, timeout=10)
        assert closed.returncode == 0, (
            f"closed-peer host exit={closed.returncode}; "
            f"stderr={closed.stderr.decode(errors='replace')}"
        )
        deadline = subprocess.run(
            [harness, "deadline"], capture_output=True, timeout=10
        )
        assert deadline.returncode == 0, (
            f"deadline host exit={deadline.returncode}; "
            f"stderr={deadline.stderr.decode(errors='replace')}"
        )
        tls_closed = subprocess.run(
            [harness, "tls-closed"], capture_output=True, timeout=15
        )
        assert tls_closed.returncode == 0, (
            f"closed TLS peer host exit={tls_closed.returncode}; "
            f"stderr={tls_closed.stderr.decode(errors='replace')}"
        )
    print("ok  native transport preserves host SIGPIPE state and total deadlines")
    print("ok  TLS handshake and close paths cannot terminate the embedding host")
    print("ok  Authorization-bearing native request buffers are securely wiped")


if __name__ == "__main__":
    main()
