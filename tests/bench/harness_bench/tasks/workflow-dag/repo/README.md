Implement workflow.py and workflow_cli.py using only the Python standard library.
simulate(tasks, workers=2, retries=1) executes a deterministic, discrete-time
DAG simulation. Input tasks is a list of 0..32 dictionaries. Each task has required
'id' (ASCII letter then up to 31 letters/digits/underscore/hyphen), 'duration'
(int excluding bool, 1..1000), optional 'depends_on' (list of distinct task IDs,
default []), and optional 'failures' (int excluding bool, 0..8, default 0). Unknown
keys, duplicate IDs, missing/self/duplicate dependencies, cycles, non-list input,
wrong types or bounds raise ValueError BEFORE simulating. workers must be an int
excluding bool in 1..8; retries an int excluding bool in 0..8.

Tasks start at time zero if dependencies permit. At each scheduling time, first
complete ALL running attempts whose finish time equals now, in lexicographic task
ID order. Attempt n fails iff n <= task.failures. After a failed attempt, retry
if n <= retries, else fail terminally. After processing all completions, mark
pending tasks whose dependency has failed or been skipped as skipped; repeat in
lexicographic waves until no further task is skipped. Then fill vacant worker
slots from ready pending tasks in lexicographic ID order (retries compete normally
with other ready tasks). A task becomes ready only after every dependency succeeds.
Each attempt runs for its task's duration. No same-task overlapping attempts.
Advance to the next completion time; stop when all tasks are terminal.

Return {"makespan": integer, "tasks": {ID: {"state": "succeeded"|"failed"|"skipped",
"attempts": integer, "finish": integer}}, "events": [...]}. finish is time of
terminal success/failure/skip. A skipped task has attempts 0. Retry and failed events carry the attempt number that just ended.
Each event has exactly
{"time": integer, "task": ID, "attempt": integer, "type": TYPE}; TYPE is start,
succeeded, retry, failed, or skipped. Skipped event attempt is 0. Emit completions
(retry/succeeded/failed), then skip waves, then starts, in the specified order.
Empty input returns makespan 0, tasks {}, events []. Do not mutate the input.

python workflow_cli.py reads ONE JSON object from stdin with required tasks and
optional workers/retries (same defaults); unknown keys or invalid input produce
{"error":"ValueError"} and exit 2. Otherwise output the simulation JSON and exit 0.
No extra stdout. Do not change the public test. Add and run your own tests.
