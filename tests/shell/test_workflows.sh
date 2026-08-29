#!/usr/bin/env bash
# Run explicitly under both Bash and Zsh from `make test-shell-workflows`.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
root=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
# Resolved from the checked-out repository root.
# shellcheck disable=SC1091
. "$root/shell/tny-workflows.sh"

temporary=$(mktemp -d "${TMPDIR:-/tmp}/tny-workflow-test.XXXXXX")
scheduler_launcher_pid=
scheduler_pid=

valid_fixture_pid() {
    case ${1:-} in
        '' | *[!0-9]*) return 1 ;;
        *) [ "$1" -gt 1 ] 2> /dev/null ;;
    esac
}

signal_recorded_groups() {
    local signal child_file child_pid
    signal=$1
    [ -n "${TNY_WORKFLOW_DIR:-}" ] || return 0
    [ -d "$TNY_WORKFLOW_DIR/run" ] || return 0
    find "$TNY_WORKFLOW_DIR/run" -name child_pid -type f -print 2> /dev/null |
        while IFS= read -r child_file; do
            child_pid=$(cat "$child_file" 2> /dev/null || true)
            valid_fixture_pid "$child_pid" || continue
            kill "-$signal" -- "-$child_pid" 2> /dev/null || true
        done
}

signal_recorded_workers() {
    local signal task worker_pid
    signal=$1
    [ -n "${TNY_WORKFLOW_DIR:-}" ] || return 0
    [ -f "$TNY_WORKFLOW_DIR/tasks.list" ] || return 0
    while IFS= read -r task || [ -n "$task" ]; do
        [ -n "$task" ] || continue
        [ -f "$TNY_WORKFLOW_DIR/run/$task/pid" ] || continue
        worker_pid=$(cat "$TNY_WORKFLOW_DIR/run/$task/pid" 2> /dev/null || true)
        valid_fixture_pid "$worker_pid" || continue
        kill "-$signal" "$worker_pid" 2> /dev/null || true
    done < "$TNY_WORKFLOW_DIR/tasks.list"
}

recorded_worker_alive() {
    local task worker_pid
    [ -n "${TNY_WORKFLOW_DIR:-}" ] || return 1
    [ -f "$TNY_WORKFLOW_DIR/tasks.list" ] || return 1
    while IFS= read -r task || [ -n "$task" ]; do
        [ -n "$task" ] || continue
        [ -f "$TNY_WORKFLOW_DIR/run/$task/pid" ] || continue
        worker_pid=$(cat "$TNY_WORKFLOW_DIR/run/$task/pid" 2> /dev/null || true)
        if valid_fixture_pid "$worker_pid" && kill -0 "$worker_pid" 2> /dev/null; then
            return 0
        fi
    done < "$TNY_WORKFLOW_DIR/tasks.list"
    return 1
}

cleanup() {
    if valid_fixture_pid "${scheduler_pid:-}"; then
        kill -TERM "$scheduler_pid" 2> /dev/null || true
    fi
    signal_recorded_groups TERM
    attempt=0
    while valid_fixture_pid "${scheduler_pid:-}" && kill -0 "$scheduler_pid" 2> /dev/null; do
        attempt=$((attempt + 1))
        [ "$attempt" -lt 30 ] || break
        sleep 0.05
    done
    signal_recorded_groups KILL
    if valid_fixture_pid "${scheduler_pid:-}"; then
        kill -KILL "$scheduler_pid" 2> /dev/null || true
    fi
    if valid_fixture_pid "${scheduler_launcher_pid:-}"; then
        kill -TERM "$scheduler_launcher_pid" 2> /dev/null || true
        wait "$scheduler_launcher_pid" 2> /dev/null || true
    fi
    attempt=0
    while recorded_worker_alive; do
        attempt=$((attempt + 1))
        [ "$attempt" -lt 30 ] || break
        sleep 0.05
    done
    signal_recorded_workers KILL
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
printf '%s\n' "${TNY_WORKFLOW_TASK_DIR:-}" > "$TNY_FAKE_LOG.task-path.$task"

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

uncooperative="$temporary/uncooperative-tny"
cat > "$uncooperative" << 'UNCOOPERATIVE'
#!/bin/sh
set -eu
: "${TNY_UNCOOPERATIVE_LOG:?}"
trap '' HUP INT TERM
printf '%s\n' "$$" > "$TNY_UNCOOPERATIVE_LOG/parent"
sh -c '
    trap "" HUP INT TERM
    printf "%s\n" "$$" > "$1/grandchild"
    while :; do sleep 1; done
' _ "$TNY_UNCOOPERATIVE_LOG" &
while [ ! -f "$TNY_UNCOOPERATIVE_LOG/grandchild" ]; do sleep 0.01; done
: > "$TNY_UNCOOPERATIVE_LOG/ready"
while :; do sleep 1; done
UNCOOPERATIVE
chmod 755 "$uncooperative"

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
tny_task evidence --stdin << 'PROMPT'
TASK evidence
DELAY 0
PROMPT
tny_task merge --after research --after tests --no-context --after evidence --stdin << 'PROMPT'
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
assert_eq "$(tny_status evidence)" success
assert_eq "$(tny_status merge)" success
assert_eq "$(tny_status ordered)" success
assert_eq "$(tny_result merge)" result:merge
[ -f "$(tny_result_path merge)" ] || fail "tny_result_path did not name a file"
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" '<tny_workflow_dependencies>'
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" '<dependency name="research">'
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" 'result:research'
assert_file_not_contains "$TNY_FAKE_LOG.prompt.merge" '<dependency name="tests">'
assert_file_not_contains "$TNY_FAKE_LOG.prompt.merge" 'result:tests'
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" '<dependency name="evidence">'
assert_file_contains "$TNY_FAKE_LOG.prompt.merge" 'result:evidence'
research_context=$(grep -n '<dependency name="research">' "$TNY_FAKE_LOG.prompt.merge" | cut -d: -f1)
evidence_context=$(grep -n '<dependency name="evidence">' "$TNY_FAKE_LOG.prompt.merge" | cut -d: -f1)
[ "$research_context" -lt "$evidence_context" ] || fail "included dependency outputs lost declaration order"
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

# Runtime-owned built-in selectors and workflow-local compatibility types.
reset_log "$temporary/scenario-task-types"
tny_workflow_begin "$temporary/flow-task-types" > /dev/null
types=$(tny_task_types)
printf '%s\n' "$types" | grep -F "review" > /dev/null || fail "review task type missing"
printf '%s\n' "$types" | grep -F "optimizer" > /dev/null || fail "optimizer task type missing"
printf '%s\n' "$types" | grep -F "document" > /dev/null || fail "document task type missing"
printf '%s\n' "$types" | grep -F "retro" > /dev/null || fail "retro task type missing"
tny_task_type security --stdin << 'TYPE'
Act as a repository security reviewer. Verify trust boundaries and report concrete risks.
TYPE
printf '%s\n' "$(tny_task_types)" | grep -F "security" > /dev/null || fail "custom task type missing"
ln -s "$temporary" "$TNY_WORKFLOW_DIR/task-types/symlinked"
if tny_task_type symlinked "must not replace a symlink" 2> "$temporary/symlink.err"; then
    fail "symlinked task type was accepted"
fi
assert_file_contains "$temporary/symlink.err" "refusing symlinked task type 'symlinked'"
long_task_name=$(printf '%064d' 0)
if tny_task_type "$long_task_name" "too long" 2> "$temporary/long-task.err"; then
    fail "64-byte task type name was accepted"
fi
if command -v iconv > /dev/null 2>&1; then
    if printf '\377' | tny_task_type invalid-utf8 --stdin 2> "$temporary/utf8-task.err"; then
        fail "invalid UTF-8 task type was accepted"
    fi
    assert_file_contains "$temporary/utf8-task.err" "must be valid UTF-8"
fi

tny_task reviewer --task review --system-prompt "Focus on the changed public API." --stdin << 'PROMPT'
TASK reviewer
PROMPT
tny_task optimizer --task optimizer --stdin << 'PROMPT'
TASK optimizer
PROMPT
tny_task documenter --task document --stdin << 'PROMPT'
TASK documenter
PROMPT
tny_task retrospective --task retro --stdin << 'PROMPT'
TASK retrospective
PROMPT
tny_task security --task security --stdin << 'PROMPT'
TASK security
PROMPT
tny_task unknown --task does-not-exist -- "TASK unknown"

TNY_WORKFLOW_TASK_DIR="$temporary/ambient-task-dir-that-must-not-leak"
export TNY_WORKFLOW_TASK_DIR
tny_workflow_run --jobs 5 --quiet || fail "task type workflow returned non-zero"
assert_file_contains "$TNY_FAKE_LOG.args.reviewer" '--task'
assert_file_contains "$TNY_FAKE_LOG.args.reviewer" 'review'
assert_eq "$(cat "$TNY_FAKE_LOG.task-path.reviewer")" ""
assert_file_not_contains "$TNY_FAKE_LOG.args.reviewer" 'Act as a rigorous code reviewer.'
assert_file_not_contains "$TNY_FAKE_LOG.args.reviewer" 'Additional task instructions:'
assert_file_contains "$TNY_FAKE_LOG.args.reviewer" 'Focus on the changed public API.'
assert_file_contains "$TNY_FAKE_LOG.args.optimizer" '--task'
assert_file_contains "$TNY_FAKE_LOG.args.optimizer" 'optimizer'
assert_file_not_contains "$TNY_FAKE_LOG.args.optimizer" 'performance and complexity optimizer'
assert_file_contains "$TNY_FAKE_LOG.args.documenter" 'document'
assert_file_not_contains "$TNY_FAKE_LOG.args.documenter" 'documentation expert'
assert_file_contains "$TNY_FAKE_LOG.args.retrospective" 'retro'
assert_file_not_contains "$TNY_FAKE_LOG.args.retrospective" 'optionally update AGENTS.md'
assert_file_contains "$TNY_FAKE_LOG.args.security" '--task'
assert_file_contains "$TNY_FAKE_LOG.args.security" 'security'
assert_file_not_contains "$TNY_FAKE_LOG.args.security" 'repository security reviewer'
assert_file_contains "$TNY_FAKE_LOG.args.unknown" 'does-not-exist'
assert_eq "$(cat "$TNY_FAKE_LOG.task-path.security")" "$TNY_WORKFLOW_DIR/task-types"
unset TNY_WORKFLOW_TASK_DIR
assert_eq "$(cat "$TNY_WORKFLOW_DIR/tasks/reviewer/task_type")" review
assert_eq "$(cat "$TNY_WORKFLOW_DIR/tasks/security/task_type")" security

# With a built binary, prove the workflow bridge reaches runtime resolution:
# unknown selectors fail as unknown, while a workflow-local custom selector
# resolves and reaches the (deliberately unavailable) provider endpoint.
if [ -x "$root/build/tny" ]; then
    mkdir -p "$temporary/real-workspace"
    tny_workflow_begin "$temporary/flow-real-task-resolution" > /dev/null
    tny_task_type local-review "Workflow local body marker."
    tny_task custom --provider openai --task local-review --cwd "$temporary/real-workspace" -- "custom"
    tny_task unknown-real --provider openai --task does-not-exist --cwd "$temporary/real-workspace" -- "unknown"
    TNY_WORKFLOW_TNY="$root/build/tny"
    OPENAI_BASE_URL=http://127.0.0.1:1/v1
    OPENAI_API_KEY=
    export TNY_WORKFLOW_TNY OPENAI_BASE_URL OPENAI_API_KEY
    if tny_workflow_run --jobs 2 --quiet; then
        fail "real task resolution fixture unexpectedly succeeded"
    fi
    assert_eq "$(tny_status custom)" failed
    assert_eq "$(tny_status unknown-real)" failed
    assert_file_not_contains "$TNY_WORKFLOW_DIR/run/custom/stderr" "unknown or invalid task 'local-review'"
    assert_file_contains "$TNY_WORKFLOW_DIR/run/unknown-real/stderr" "unknown or invalid task 'does-not-exist'"
    tny_workflow_cleanup
    unset OPENAI_BASE_URL OPENAI_API_KEY
    TNY_WORKFLOW_TNY=$fake
    export TNY_WORKFLOW_TNY
fi

# Failed tasks block descendants but do not cancel independent branches.
reset_log "$temporary/scenario-two"
tny_workflow_begin "$temporary/flow-two" > /dev/null
# Define this chain in reverse topological order. Blocking must propagate to a
# fixed point in the same scheduler pass after the rapid root failure.
tny_task grandchild --after child -- "TASK grandchild"
tny_task child --after bad -- "TASK child"
tny_task independent --stdin << 'PROMPT'
TASK independent
DELAY 0.10
PROMPT
tny_task bad --stdin << 'PROMPT'
TASK bad
FAIL
PROMPT
set +e
tny_workflow_run -j 2 --quiet
workflow_rc=$?
set -e
assert_eq "$workflow_rc" 1 "rapid failure workflow status"
assert_eq "$(tny_status bad)" failed
assert_eq "$(tny_status independent)" success
assert_eq "$(tny_status child)" blocked
assert_eq "$(tny_status grandchild)" blocked
assert_eq "$(cat "$TNY_WORKFLOW_DIR/run/bad/exit_code")" 7
[ ! -f "$TNY_FAKE_LOG.prompt.child" ] || fail "blocked child was launched"
assert_file_contains "$TNY_WORKFLOW_DIR/run/child/stderr" "dependency 'bad' is failed"

tny_workflow_begin "$temporary/flow-edge-validation" > /dev/null
if tny_task invalid --no-context --after independent -- "TASK invalid" 2> "$temporary/edge-validation.err"; then
    fail "--no-context without a preceding edge was accepted"
fi
assert_file_contains "$temporary/edge-validation.err" '--no-context must immediately follow --after NAME'

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
# Expanded by the child scheduler shell, not this test process.
# shellcheck disable=SC2016
scheduler_program='. "$1"; TNY_WORKFLOW_DIR=$2; export TNY_WORKFLOW_DIR; _tny_workflow_run_impl --quiet'
start_scheduler() {
    local pid_file attempt
    pid_file=$1
    rm -f "$pid_file"
    # Zsh may put an asynchronous `exec` behind a short-lived job wrapper, so
    # its outer $! is not reliably the process that installed scheduler traps.
    # Keep a supervisor alive, record its direct child, signal that exact PID,
    # and wait on the supervisor job for the scheduler's final status.
    sh -c '
        "$1" -c "$2" _ "$3" "$4" &
        scheduler=$!
        printf "%s\n" "$scheduler" > "$5"
        wait "$scheduler"
        exit $?
    ' _ "$current_shell" "$scheduler_program" "$root/shell/tny-workflows.sh" \
        "$TNY_WORKFLOW_DIR" "$pid_file" &
    scheduler_launcher_pid=$!
    attempt=0
    while [ ! -s "$pid_file" ]; do
        kill -0 "$scheduler_launcher_pid" 2> /dev/null || fail "scheduler supervisor exited before publishing its PID"
        attempt=$((attempt + 1))
        [ "$attempt" -lt 300 ] || fail "scheduler supervisor did not publish its PID"
        sleep 0.01
    done
    scheduler_pid=$(cat "$pid_file")
    valid_fixture_pid "$scheduler_pid" || fail "scheduler supervisor published an invalid PID"
}

start_scheduler "$temporary/signal-scheduler.pid"
attempt=0
while ! grep -Fqx 'start slow' "$TNY_FAKE_LOG.events" 2> /dev/null; do
    kill -0 "$scheduler_pid" 2> /dev/null || fail "scheduler exited before the signal fixture started"
    attempt=$((attempt + 1))
    [ "$attempt" -lt 300 ] || fail "signal fixture did not start"
    sleep 0.01
done
kill -TERM "$scheduler_pid"
set +e
wait "$scheduler_launcher_pid"
scheduler_rc=$?
set -e
assert_eq "$scheduler_rc" 130 "scheduler signal exit"
if kill -0 "$scheduler_pid" 2> /dev/null; then
    fail "scheduler survived its signal fixture"
fi
scheduler_pid=
scheduler_launcher_pid=
assert_eq "$(cat "$TNY_FAKE_LOG.active")" 0 "active child after scheduler signal"
assert_file_contains "$TNY_FAKE_LOG.events" 'end slow'
tny_workflow_cleanup

# Cancellation targets the entire child process group and escalates after a
# bounded TERM grace period when both the command and its descendant ignore it.
TNY_UNCOOPERATIVE_LOG="$temporary/scenario-uncooperative"
export TNY_UNCOOPERATIVE_LOG
mkdir -p "$TNY_UNCOOPERATIVE_LOG"
TNY_WORKFLOW_TNY=$uncooperative
export TNY_WORKFLOW_TNY
tny_workflow_begin "$temporary/flow-uncooperative" > /dev/null
tny_task stubborn -- "TASK stubborn"
start_scheduler "$temporary/uncooperative-scheduler.pid"
attempt=0
while [ ! -f "$TNY_UNCOOPERATIVE_LOG/ready" ]; do
    kill -0 "$scheduler_pid" 2> /dev/null || fail "uncooperative scheduler exited before fixture readiness"
    attempt=$((attempt + 1))
    [ "$attempt" -lt 300 ] || fail "uncooperative fixture did not start"
    sleep 0.01
done
stubborn_pid=$(cat "$TNY_UNCOOPERATIVE_LOG/parent")
grandchild_pid=$(cat "$TNY_UNCOOPERATIVE_LOG/grandchild")
assert_eq "$(tr -d ' ' < "$TNY_WORKFLOW_DIR/run/stubborn/child_pid")" "$stubborn_pid" "process group leader"
assert_eq "$(ps -o pgid= -p "$grandchild_pid" | tr -d ' ')" "$stubborn_pid" "grandchild process group"
started_at=$(date +%s)
kill -TERM "$scheduler_pid"
set +e
wait "$scheduler_launcher_pid"
scheduler_rc=$?
set -e
elapsed=$(($(date +%s) - started_at))
assert_eq "$scheduler_rc" 130 "uncooperative scheduler signal exit"
if kill -0 "$scheduler_pid" 2> /dev/null; then
    fail "scheduler survived uncooperative cancellation"
fi
scheduler_pid=
scheduler_launcher_pid=
[ "$elapsed" -lt 5 ] || fail "uncooperative cancellation exceeded bounded grace (${elapsed}s)"
attempt=0
while kill -0 "$stubborn_pid" 2> /dev/null || kill -0 "$grandchild_pid" 2> /dev/null; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || fail "uncooperative process group survived cancellation"
    sleep 0.01
done
tny_workflow_cleanup
TNY_WORKFLOW_TNY=$fake
export TNY_WORKFLOW_TNY

printf 'ok: shell workflows (%s)\n' "${ZSH_VERSION:+zsh ${ZSH_VERSION}}${BASH_VERSION:+bash ${BASH_VERSION}}"
