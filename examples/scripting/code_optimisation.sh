#!/usr/bin/env bash
# Fan out two grok reviews, then fan in to a grok task that turns the review
# findings into task files. Reviews run concurrently; generate-tasks waits for
# both and receives their output as context (docs/workflows.md).
set -eu

# shellcheck source=/dev/null
. "${TNY_PREFIX:-$HOME/.local}/share/tny/tny-workflows.sh"

tny_workflow_begin
trap 'tny_workflow_cleanup' EXIT

tny_task review-tests --task review --provider grok -- \
    "Review the depth, breadth and validity of the tests in this project."

tny_task review-complexity --task review --provider grok -- \
    "Identify code that is overly complex when a simpler solution would deliver the same value. Identify areas of code with high cyclomatic complexity that could be improved."

tny_task generate-tasks --provider grok \
    --after review-tests --after review-complexity -- \
    "Create tasks inside the 'tasks/' directory that capture the required actions from the reviews run against this project."

if ! tny_workflow_run --jobs 2; then
    tny_workflow_report >&2
    tny_stderr generate-tasks >&2 || true
    exit 1
fi

tny_result generate-tasks
