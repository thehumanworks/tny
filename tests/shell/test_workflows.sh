#!/usr/bin/env bash
# Run explicitly under both Bash and Zsh from `make test-shell-workflows`.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
root=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
# Resolved from the checked-out repository root.
# shellcheck disable=SC1091
. "$root/shell/tny-workflows.sh"

temporary=$(mktemp -d "${TMPDIR:-/tmp}/tny-workflow-test.XXXXXX")
cleanup() {
    rm -r -f "$temporary"
}
trap cleanup EXIT HUP INT TERM

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

assert_eq() {
    [ "$1" = "$2" ] || fail "expected '$2', got '$1'${3:+ ($3)}"
}

assert_file_contains() {
    grep -F -- "$2" "$1" > /dev/null || fail "$1 does not contain: $2"
}

assert_file_not_contains() {
    if grep -F -- "$2" "$1" > /dev/null 2>&1; then
        fail "$1 unexpectedly contains: $2"
    fi
}

fake="$temporary/fake-tny"
cat > "$fake" << 'FAKE'
#!/bin/sh
set -eu
: "${TNY_FAKE_LOG:?}"

lock="$TNY_FAKE_LOG.lock"
acquire() {
    while ! mkdir "$lock" 2> /dev/null; do sleep 0.01; done
}
release() {
    rmdir "$lock"
}

prompt_file="$TNY_FAKE_LOG.prompt.$$"
cat > "$prompt_file"
task=$(sed -n '1s/^TASK //p' "$prompt_file")
[ -n "$task" ] || task=unknown
mv "$prompt_file" "$TNY_FAKE_LOG.prompt.$task"
printf '%s\n' "$@" > "$TNY_FAKE_LOG.args.$task"

delay=$(sed -n 's/^DELAY //p' "$TNY_FAKE_LOG.prompt.$task" | sed -n '1p')
[ -n "$delay" ] || delay=0

active_released=0
finish_active() {
    [ "$active_released" -eq 0 ] || return 0
    acquire
    active=$(cat "$TNY_FAKE_LOG.active")
    active=$((active - 1))
    printf '%s\n' "$active" > "$TNY_FAKE_LOG.active"
    printf 'end %s\n' "$task" >> "$TNY_FAKE_LOG.events"
    release
    active_released=1
}
trap finish_active EXIT
trap 'finish_active; exit 143' HUP INT TERM

acquire
active=$(cat "$TNY_FAKE_LOG.active")
active=$((active + 1))
printf '%s\n' "$active" > "$TNY_FAKE_LOG.active"
maximum=$(cat "$TNY_FAKE_LOG.maximum")
if [ "$active" -gt "$maximum" ]; then
    printf '%s\n' "$active" > "$TNY_FAKE_LOG.maximum"
fi
printf 'start %s\n' "$task" >> "$TNY_FAKE_LOG.events"
release

sleep "$delay"
if grep -Fqx 'FAIL' "$TNY_FAKE_LOG.prompt.$task"; then
    printf 'fake failure for %s\n' "$task" >&2
    exit 7
fi
printf 'result:%s\n' "$task"
FAKE
chmod 755 "$fake"

reset_log() {
    mkdir -p "$1"
    TNY_FAKE_LOG="$1/log"
    export TNY_FAKE_LOG
    : > "$TNY_FAKE_LOG.events"
    printf '%s\n' 0 > "$TNY_FAKE_LOG.active"
    printf '%s\n' 0 > "$TNY_FAKE_LOG.maximum"
}

TNY_WORKFLOW_TNY=$fake
export TNY_WORKFLOW_TNY

# Parallel roots, deterministic dependency context, option fidelity, and a
# sequencing-only edge that suppresses dependency output.
reset_log "$temporary/scenario-one"
tny_workflow_begin "$temporary/flow-one" > /dev/null
mkdir -p "$temporary/work space"
tny_task research \
    --provider codex \
    --model model-one \
    --effort high \
    --cwd "$temporary/work space" \
    --system-prompt "system prompt with spaces" \
    --permission-mode auto \
    --max-steps 17 \
    --ssh user@example:2222 \
    --ssh-cwd '/srv/remote worktree' \
    --agent fixture-agent \
    --fast \
    --stdin << 'PROMPT'
TASK research
DELAY 0.20
PROMPT
tny_task tests --stdin << 'PROMPT'
TASK tests
DELAY 0.20
PROMPT
tny_task merge --after research --after tests --stdin << 'PROMPT'
TASK merge
DELAY 0
PROMPT
tny_task ordered --after merge --no-context --stdin << 'PROMPT'
TASK ordered
DELAY 0
PROMPT

tny_workflow_run --jobs 2 --quiet || fail "successful workflow returned non-zero"
assert_eq "$(cat "$TNY_FAKE_LOG.maximum")" 2 "parallel roots"
assert_eq "$(tny_status research)" success
assert_eq "$(tny_status tests)" success
assert_eq "$(tny_status merge)" success
assert_eq "$(tny_status ordered)" success
assert_eq "$(tny_result merge)" result:merge
[ -f "$(tny_result_path merge)" ] || fail "tny_result_path did not name a file"
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" '<tny_workflow_dependencies>'
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" '<dependency name="research">'
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" 'result:research'
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" 'result:tests'
assert_file_not_contains "$TNY_FAKE_LOG.prompt.ordered" '<tny_workflow_dependencies>'
assert_file_contains "$TNY_FAKE_LOG.args.research" '--provider'
assert_file_contains "$TNY_FAKE_LOG.args.research" 'codex'
assert_file_contains "$TNY_FAKE_LOG.args.research" 'system prompt with spaces'
assert_file_contains "$TNY_FAKE_LOG.args.research" 'user@example:2222'
assert_file_contains "$TNY_FAKE_LOG.args.research" '--ssh-cwd'
assert_file_contains "$TNY_FAKE_LOG.args.research" '/srv/remote worktree'
assert_file_contains "$TNY_FAKE_LOG.args.research" '--ephemeral'
assert_file_contains "$TNY_FAKE_LOG.args.research" '--stdin'
report=$(tny_workflow_report)
expected_report_line=$(printf 'merge\tsuccess\t0')
printf '%s\n' "$report" | grep -F "$expected_report_line" > /dev/null || fail "report missing merge success"
research_end=$(grep -n '^end research$' "$TNY_FAKE_LOG.events" | cut -d: -f1)
tests_end=$(grep -n '^end tests$' "$TNY_FAKE_LOG.events" | cut -d: -f1)
merge_start=$(grep -n '^start merge$' "$TNY_FAKE_LOG.events" | cut -d: -f1)
[ "$merge_start" -gt "$research_end" ] || fail "merge started before research ended"
[ "$merge_start" -gt "$tests_end" ] || fail "merge started before tests ended"

# Failed tasks block descendants but do not cancel independent branches.
reset_log "$temporary/scenario-two"
tny_workflow_begin "$temporary/flow-two" > /dev/null
tny_task bad --stdin << 'PROMPT'
TASK bad
DELAY 0.05
FAIL
PROMPT
tny_task independent --stdin << 'PROMPT'
TASK independent
DELAY 0.10
PROMPT
tny_task child --after bad -- "TASK child"
tny_task grandchild --after child -- "TASK grandchild"
if tny_workflow_run -j 2 --quiet; then
    fail "failing workflow returned zero"
fi
assert_eq "$(tny_status bad)" failed
assert_eq "$(tny_status independent)" success
assert_eq "$(tny_status child)" blocked
assert_eq "$(tny_status grandchild)" blocked
assert_eq "$(cat "$TNY_WORKFLOW_DIR/run/bad/exit_code")" 7
[ ! -f "$TNY_FAKE_LOG.prompt.child" ] || fail "blocked child was launched"
assert_file_contains "$TNY_WORKFLOW_DIR/run/child/stderr" "dependency 'bad' is failed"

# Missing edges and cycles fail validation before any process starts.
reset_log "$temporary/scenario-three"
tny_workflow_begin "$temporary/flow-three" > /dev/null
tny_task orphan --after absent -- "TASK orphan"
if tny_workflow_run --quiet 2> "$temporary/missing.err"; then
    fail "undefined dependency was accepted"
fi
assert_file_contains "$temporary/missing.err" "depends on undefined task 'absent'"
[ ! -s "$TNY_FAKE_LOG.events" ] || fail "undefined DAG launched a task"

reset_log "$temporary/scenario-four"
tny_workflow_begin "$temporary/flow-four" > /dev/null
tny_task first --after second -- "TASK first"
tny_task second --after first -- "TASK second"
if tny_workflow_run --quiet 2> "$temporary/cycle.err"; then
    fail "cycle was accepted"
fi
assert_file_contains "$temporary/cycle.err" 'dependency cycle detected'
[ ! -s "$TNY_FAKE_LOG.events" ] || fail "cyclic DAG launched a task"

# Context limits fail the consumer task without rerunning a completed producer.
reset_log "$temporary/scenario-five"
tny_workflow_begin "$temporary/flow-five" > /dev/null
tny_task producer -- "TASK producer"
tny_task consumer --after producer -- "TASK consumer"
TNY_WORKFLOW_MAX_DEPENDENCY_BYTES=5
export TNY_WORKFLOW_MAX_DEPENDENCY_BYTES
if tny_workflow_run --quiet; then
    fail "dependency context limit was ignored"
fi
unset TNY_WORKFLOW_MAX_DEPENDENCY_BYTES
assert_eq "$(tny_status producer)" success
assert_eq "$(tny_status consumer)" failed
[ ! -f "$TNY_FAKE_LOG.prompt.consumer" ] || fail "oversized consumer reached tny"
assert_file_contains "$TNY_WORKFLOW_DIR/run/consumer/stderr" 'exceeds 5 bytes'

# Definition validation and cleanup avoid ambiguous or destructive state.
tny_workflow_begin "$temporary/flow-six" > /dev/null
tny_task once -- "TASK once"
if tny_task once -- "TASK duplicate" 2> "$temporary/duplicate.err"; then
    fail "duplicate task was accepted"
fi
assert_file_contains "$temporary/duplicate.err" 'already defined'
if tny_task '../escape' -- "TASK escape" 2> "$temporary/name.err"; then
    fail "unsafe task name was accepted"
fi
assert_file_contains "$temporary/name.err" 'invalid task name'
if tny_result_path '../escape' > /dev/null 2> "$temporary/result-name.err"; then
    fail "tny_result_path accepted an unsafe task name"
fi
assert_file_contains "$temporary/result-name.err" 'unknown task'
if tny_stderr '../escape' > /dev/null 2> "$temporary/stderr-name.err"; then
    fail "tny_stderr accepted an unsafe task name"
fi
assert_file_contains "$temporary/stderr-name.err" 'unknown task'
explicit=$TNY_WORKFLOW_DIR
tny_workflow_cleanup
[ -d "$explicit" ] || fail "explicit workflow directory was removed"
[ ! -e "$explicit/.tny-workflow" ] || fail "explicit workflow marker survived cleanup"

# Scheduler termination reaches every active worker and waits for its tny child.
reset_log "$temporary/scenario-signal"
tny_workflow_begin "$temporary/flow-signal" > /dev/null
tny_task slow --stdin << 'PROMPT'
TASK slow
DELAY 5
PROMPT
case ${ZSH_VERSION:-} in
    '') current_shell=${BASH:-bash} ;;
    *) current_shell=${ZSH:-zsh} ;;
esac
# The single-quoted program is intentionally expanded by the child shell.
# shellcheck disable=SC2016
"$current_shell" -c '. "$1"; TNY_WORKFLOW_DIR=$2; export TNY_WORKFLOW_DIR; _tny_workflow_run_impl --quiet' \
    _ "$root/shell/tny-workflows.sh" "$TNY_WORKFLOW_DIR" &
scheduler_pid=$!
attempt=0
while ! grep -Fqx 'start slow' "$TNY_FAKE_LOG.events" 2> /dev/null; do
    kill -0 "$scheduler_pid" 2> /dev/null || fail "scheduler exited before the signal fixture started"
    attempt=$((attempt + 1))
    [ "$attempt" -lt 300 ] || fail "signal fixture did not start"
    sleep 0.01
done
kill -TERM "$scheduler_pid"
set +e
wait "$scheduler_pid"
scheduler_rc=$?
set -e
assert_eq "$scheduler_rc" 130 "scheduler signal exit"
assert_eq "$(cat "$TNY_FAKE_LOG.active")" 0 "active child after scheduler signal"
assert_file_contains "$TNY_FAKE_LOG.events" 'end slow'
tny_workflow_cleanup

printf 'ok: shell workflows (%s)\n' "${ZSH_VERSION:+zsh ${ZSH_VERSION}}${BASH_VERSION:+bash ${BASH_VERSION}}"
