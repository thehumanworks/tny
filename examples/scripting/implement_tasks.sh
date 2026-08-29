#!/usr/bin/env bash
# Implement task files in parallel git worktrees, one branch per task, then
# integrate every finished branch into main and push (docs/workflows.md).
#
# Per task:   plan -> contract -> implement -> qa -> document -> commit+push
# Fan-in:     integrate (merge branches, rerun gates, move tasks to done, push)
#
# Usage:
#   examples/scripting/implement_tasks.sh [OPTIONS] [TASK.md ...]
#     default TASK set: tasks/[0-9]*.md (not tasks/doing or tasks/done)
#   --tasks 08,10,15  run only these task numbers (matches tasks/<NN>_*.md)
#   --jobs N        max concurrent agent processes (default 3)
#   --no-push       merge into local main but do not push to origin/main
#   --plan          print the task graph and exit without running agents
#   --keep          keep worktrees after the run (default: removed on success)
#
# Providers/effort per stage are env-overridable:
#   TNY_PLAN_PROVIDER TNY_IMPL_PROVIDER TNY_QA_PROVIDER TNY_DOC_PROVIDER
#   TNY_INTEGRATE_PROVIDER; defaults: grok for plan/contract/docs, codex for
#   implement/qa/integrate. TNY_IMPL_EFFORT (high)
set -eu

# shellcheck source=/dev/null
. "${TNY_PREFIX:-$HOME/.local}/share/tny/tny-workflows.sh"

here=$(cd "$(dirname "$0")" && pwd)
repo=$(git rev-parse --show-toplevel)
cd "$repo"
types="$here/task-types"

jobs=${TNY_WORKFLOW_JOBS:-3}
push=1
plan_only=0
keep=0
tasks=()
numbers=
while [ $# -gt 0 ]; do
    case "$1" in
        --tasks)
            numbers=$2
            shift 2
            ;;
        --jobs)
            jobs=$2
            shift 2
            ;;
        --no-push)
            push=0
            shift
            ;;
        --plan)
            plan_only=1
            shift
            ;;
        --keep)
            keep=1
            shift
            ;;
        --)
            shift
            tasks+=("$@")
            break
            ;;
        -*)
            echo "unknown option: $1" >&2
            exit 2
            ;;
        *)
            tasks+=("$1")
            shift
            ;;
    esac
done
if [ -n "$numbers" ]; then
    [ ${#tasks[@]} -eq 0 ] || {
        echo "--tasks cannot be combined with task file arguments" >&2
        exit 2
    }
    for n in ${numbers//,/ }; do
        matches=(tasks/"$n"_*.md)
        [ ${#matches[@]} -eq 1 ] && [ -f "${matches[0]}" ] ||
            {
                echo "task $n: expected exactly one tasks/${n}_*.md" >&2
                exit 2
            }
        tasks+=("${matches[0]}")
    done
elif [ ${#tasks[@]} -eq 0 ]; then
    for f in tasks/[0-9]*.md; do [ -f "$f" ] && tasks+=("$f"); done
fi
[ ${#tasks[@]} -gt 0 ] || {
    echo "no task files" >&2
    exit 2
}

if [ "$plan_only" -eq 0 ] && [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "main checkout has uncommitted tracked changes; commit or stash first" >&2
    exit 2
fi
git fetch origin main --quiet

plan_p=${TNY_PLAN_PROVIDER:-grok}
impl_p=${TNY_IMPL_PROVIDER:-codex}
qa_p=${TNY_QA_PROVIDER:-codex}
doc_p=${TNY_DOC_PROVIDER:-grok}
int_p=${TNY_INTEGRATE_PROVIDER:-codex}
effort=${TNY_IMPL_EFFORT:-high}

run_dir="$repo/.worktrees/run-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$run_dir"
tny_workflow_begin "$run_dir/workflow"
cleanup() {
    if [ "$keep" -eq 0 ] && [ "${status:-1}" -eq 0 ]; then
        tny_workflow_cleanup
        for wt in "$run_dir"/wt-*; do
            [ -d "$wt" ] && git worktree remove --force "$wt" || true
        done
        rm -rf "$run_dir"
    else
        echo "run directory kept (logs under workflow/run/<node>/): $run_dir" >&2
    fi
}
trap cleanup EXIT

for t in planner contract implementer qa committer integrator; do
    tny_task_type "$t" --stdin < "$types/$t.md"
done

commit_nodes=()
branches=()
for file in "${tasks[@]}"; do
    [ -f "$file" ] || {
        echo "missing task file: $file" >&2
        exit 2
    }
    slug=$(basename "$file" .md)
    branch="task/$slug"
    wt="$run_dir/wt-$slug"
    branches+=("$branch")
    if [ "$plan_only" -eq 0 ]; then
        git branch -f "$branch" origin/main
        git worktree add --quiet "$wt" "$branch"
    fi
    spec=$(cat "$file")

    tny_task "$slug.plan" --task planner --provider "$plan_p" --cwd "$wt" --stdin << PROMPT
Plan the implementation of task file $file. Its content:

$spec
PROMPT

    tny_task "$slug.contract" --task contract --provider "$plan_p" --cwd "$wt" \
        --after "$slug.plan" --stdin << PROMPT
Write the verification contract for task $file:

$spec
PROMPT

    tny_task "$slug.implement" --task implementer --provider "$impl_p" \
        --effort "$effort" --cwd "$wt" \
        --after "$slug.plan" --after "$slug.contract" --stdin << PROMPT
Implement task $file in this worktree (branch $branch):

$spec
PROMPT

    tny_task "$slug.qa" --task qa --provider "$qa_p" --cwd "$wt" \
        --after "$slug.contract" --after "$slug.implement" --stdin << PROMPT
Verify the implementation of task $file against the contract.
PROMPT

    tny_task "$slug.document" --task document --provider "$doc_p" --cwd "$wt" \
        --after "$slug.implement" --after "$slug.qa" --stdin << PROMPT
Update docs/ (and ADRs if a decision changed) so the contract matches the
implementation of task $file. Do not commit.
PROMPT

    tny_task "$slug.commit" --task committer --provider "$impl_p" --cwd "$wt" \
        --after "$slug.qa" --after "$slug.document" --stdin << PROMPT
Commit and push branch $branch for task $file.
PROMPT
    commit_nodes+=("$slug.commit")
done

int_args=()
for n in "${commit_nodes[@]}"; do int_args+=(--after "$n"); done
if [ "$push" -eq 1 ]; then
    push_line="After every gate passes, push main to origin: git push origin main."
else
    push_line="Do NOT push main; leave the merged result local."
fi
tny_task integrate --task integrator --provider "$int_p" --effort high \
    --cwd "$repo" "${int_args[@]}" --stdin << PROMPT
Integrate these branches into main, in this order: ${branches[*]}.
Start from: git checkout main && git pull --ff-only origin main.
$push_line
PROMPT

if [ "$plan_only" -eq 1 ]; then
    printf 'graph: %s tasks -> integrate; jobs=%s push=%s\n' "${#tasks[@]}" "$jobs" "$push"
    printf '  %s\n' "${branches[@]}"
    status=0
    exit 0
fi

status=0
tny_workflow_run --jobs "$jobs" || status=1
tny_workflow_report >&2
if [ "$status" -ne 0 ]; then
    for n in "${commit_nodes[@]}" integrate; do
        [ "$(tny_status "$n")" = failed ] && {
            echo "--- stderr $n" >&2
            tny_stderr "$n" >&2 || true
        }
    done
    exit 1
fi
tny_result integrate
