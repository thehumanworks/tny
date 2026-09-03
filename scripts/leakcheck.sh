#!/usr/bin/env bash
# Memory-leak gate (docs/adr/0061). Runs the sanitizer-free unit binary and the
# CLI smoke under the host's leak checker and fails on a leak:
#
#   Linux   valgrind memcheck, whole binary in one run
#   macOS   /usr/bin/leaks --atExit, suite by suite (valgrind has no arm64 port)
#   other   an honest skip, exit 0
#
# Driven by `make leaks` / `make valgrind`, which build the binaries first and
# export the variables below. Do not run it against an ASan build: the
# sanitizer replaces malloc and both checkers then see its arena, not ours.
set -eu

MODE=${1:-auto}

TEST_BIN=${TEST_BIN:-build/leakcheck/tny-test}
CLI_BIN=${CLI_BIN:-build/leakcheck/tny}
# tests that spawn the CLI (runner_suite) must find the leak-check build, not build/tny
TNY_BIN="$(pwd)/$CLI_BIN"
export TNY_BIN
VALGRIND=${VALGRIND:-valgrind}
VALGRIND_SUPP=${VALGRIND_SUPP:-tests/valgrind.supp}
VALGRIND_FLAGS=${VALGRIND_FLAGS:---leak-check=full --error-exitcode=1 --child-silent-after-fork=yes --errors-for-leak-kinds=definite,indirect --suppressions=$VALGRIND_SUPP}
LEAKS=${LEAKS:-leaks}
# The suites the macOS pass may run: every RUN_SUITE in tests/test_main.c
# minus the process-spawning ones (set by the Makefile).
LEAK_SUITES=${LEAK_SUITES:-}

cd "$(dirname "$0")/.."

if [ "$MODE" = auto ]; then
    case "$(uname -s)" in
        Linux) MODE=valgrind ;;
        Darwin) MODE=leaks ;;
        *) MODE=skip ;;
    esac
fi

fail=0

run_valgrind() {
    label=$1
    shift
    echo "== leak: $label"
    # shellcheck disable=SC2086  # VALGRIND_FLAGS is a caller-supplied word list
    if "$VALGRIND" $VALGRIND_FLAGS "$@"; then
        echo "   ok"
    else
        echo "   LEAK/ERROR: $label" >&2
        fail=1
    fi
}

run_leaks() {
    label=$1
    shift
    log="build/leakcheck/$(printf '%s' "$label" | tr -c 'A-Za-z0-9_.-' '_').log"
    mkdir -p "$(dirname "$log")"
    echo "== leak: $label"
    if "$LEAKS" --atExit -- "$@" > "$log" 2>&1; then
        # The report is the only line worth echoing; MallocStackLogging
        # writes a banner into every process it instruments.
        grep -a 'leaks for' "$log" | tail -1 || echo "   ok"
    else
        echo "   LEAK: $label ($log)" >&2
        grep -a 'leaks for' "$log" | tail -1 >&2 || true
        fail=1
    fi
}

case "$MODE" in
    valgrind)
        if ! command -v "$VALGRIND" > /dev/null 2>&1; then
            echo "error: $VALGRIND not found; apt-get install valgrind" >&2
            exit 2
        fi
        test -x "$TEST_BIN" || {
            echo "error: $TEST_BIN missing — run 'make leaks'" >&2
            exit 2
        }
        run_valgrind unit "./$TEST_BIN"
        run_valgrind "tny --version" "./$CLI_BIN" --version
        run_valgrind "tny --help" "./$CLI_BIN" --help
        run_valgrind "tny ask --help" "./$CLI_BIN" ask --help
        run_valgrind "tny doctor --json" "./$CLI_BIN" doctor --json
        ;;
    leaks)
        if ! command -v "$LEAKS" > /dev/null 2>&1; then
            echo "error: $LEAKS not found (expected /usr/bin/leaks)" >&2
            exit 2
        fi
        test -x "$TEST_BIN" || {
            echo "error: $TEST_BIN missing — run 'make leaks'" >&2
            exit 2
        }
        for suite in $LEAK_SUITES; do
            run_leaks "$suite" "./$TEST_BIN" -s "$suite" -e
        done
        run_leaks "tny --version" "./$CLI_BIN" --version
        run_leaks "tny --help" "./$CLI_BIN" --help
        run_leaks "tny ask --help" "./$CLI_BIN" ask --help
        run_leaks "tny doctor --json" "./$CLI_BIN" doctor --json
        ;;
    skip)
        echo "leaks: no leak checker on $(uname -s); Linux CI runs valgrind"
        exit 0
        ;;
    *)
        echo "error: unknown mode '$MODE' (auto|valgrind|leaks|skip)" >&2
        exit 2
        ;;
esac

if [ "$fail" -ne 0 ]; then
    echo "leaks: FAILED" >&2
    exit 1
fi
echo "leaks: clean ($MODE)"
