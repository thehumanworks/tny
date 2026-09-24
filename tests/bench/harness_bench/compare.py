"""Paired, task-clustered comparison of two recorded benchmark arms."""

import gzip
import json
import math
import random
import re
from collections import defaultdict
from pathlib import Path

from cost import request_cost

BOOTSTRAP_SEED = 20260924
BOOTSTRAP_RESAMPLES = 10_000
ARM_METRICS = (
    "ite_per_completed_task",
    "usd_per_completed_task",
    "ite_per_task",
    "usd_per_task",
    "requests_per_task",
    "mean_context_tokens",
    "output_tokens_per_task",
    "wall_s_per_task",
)
RATIO_METRICS = (
    "ite",
    "usd",
    "requests",
    "mean_context_tokens",
    "output_tokens",
    "wall_s",
)


def _wilson(passed, runs):
    z = 1.96
    rate = passed / runs
    denominator = 1 + z * z / runs
    center = (rate + z * z / (2 * runs)) / denominator
    half = (
        z
        * math.sqrt(rate * (1 - rate) / runs + z * z / (4 * runs * runs))
        / denominator
    )
    return [max(0, center - half), min(1, center + half)]


def _read_arm(directory, harness):
    directory = Path(directory).resolve()
    if not directory.is_dir():
        raise ValueError(f"run directory does not exist: {directory}")
    rows = []
    paths = sorted(
        [*directory.glob("*/*/result.json"), *directory.glob("*/*/rep-*/result.json")]
    )
    for path in paths:
        row = json.loads(path.read_text())
        if harness is None or row.get("harness") == harness:
            row["_path"] = str(path)
            rows.append(row)
    if not rows:
        raise ValueError(
            f"no result.json files for {harness or 'any harness'} in {directory}"
        )
    names = {row.get("harness") for row in rows}
    if len(names) != 1:
        raise ValueError(f"{directory} contains multiple harnesses; pass --harness")
    return rows, names.pop(), directory


def _settings(rows):
    settings = {(row.get("model"), row.get("effort")) for row in rows}
    if len(settings) != 1 or any(value is None for value in next(iter(settings))):
        raise ValueError("every run in an arm must have the same model and effort")
    return next(iter(settings))


def _number(value, label):
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{label} must be numeric")
    value = float(value)
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"{label} must be finite and nonnegative")
    return value


def _sum_known(values):
    return sum(values) if all(value is not None for value in values) else None


def _normalized(row):
    if not isinstance(row.get("pass"), bool):
        raise ValueError(f"{row['_path']}: pass must be boolean")
    if row.get("status") == "error":
        raise ValueError(f"{row['_path']}: environment error; rerun before comparison")
    task = row.get("task")
    rep = row.get("rep")
    if (
        not isinstance(task, str)
        or not task
        or (rep is not None and (not isinstance(rep, int) or isinstance(rep, bool)))
    ):
        raise ValueError(f"{row['_path']}: task or rep is invalid")
    request_rows = row.get("request_rows")
    requests = row.get(
        "requests", len(request_rows) if isinstance(request_rows, list) else None
    )
    if not isinstance(requests, int) or isinstance(requests, bool) or requests < 0:
        raise ValueError(f"{row['_path']}: requests must be a nonnegative integer")
    if isinstance(request_rows, list):
        if len(request_rows) != requests:
            raise ValueError(f"{row['_path']}: request count differs from request_rows")
        costs = [request_cost(request, row["model"]) for request in request_rows]
        ite = _sum_known([cost[0] for cost in costs])
        usd = _sum_known([cost[1] for cost in costs])
        input_tokens = _sum_known(
            [
                _number(request.get("input_tokens"), "input_tokens")
                for request in request_rows
            ]
        )
        output_tokens = _sum_known(
            [
                _number(request.get("output_tokens"), "output_tokens")
                for request in request_rows
            ]
        )
    else:
        ite = _number(row.get("ite"), "ite")
        usd = _number(row.get("usd"), "usd")
        input_tokens = _number(row.get("input_tokens"), "input_tokens")
        output_tokens = _number(row.get("output_tokens"), "output_tokens")
    return {
        "task": task,
        "rep": rep,
        "pass": row["pass"],
        "ite": ite,
        "usd": usd,
        "requests": requests,
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
        "wall_s": _number(row.get("wall_s"), "wall_s"),
    }


def _index(rows):
    indexed = {}
    for original in rows:
        row = _normalized(original)
        key = (row["task"], row["rep"])
        if key in indexed:
            raise ValueError(f"duplicate task/rep: {key}")
        indexed[key] = row
    return indexed


def _task_totals(rows):
    return {
        "runs": len(rows),
        "passes": sum(row["pass"] for row in rows),
        "ite": _sum_known([row["ite"] for row in rows]),
        "usd": _sum_known([row["usd"] for row in rows]),
        "requests": sum(row["requests"] for row in rows),
        "input_tokens": _sum_known([row["input_tokens"] for row in rows]),
        "output_tokens": _sum_known([row["output_tokens"] for row in rows]),
        "wall_s": _sum_known([row["wall_s"] for row in rows]),
    }


def _percentile(sorted_values, fraction):
    position = (len(sorted_values) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    left, right = sorted_values[low], sorted_values[high]
    if left == right or low == high:
        return left
    if not math.isfinite(left) or not math.isfinite(right):
        return right
    return left + (right - left) * (position - low)


def _ci(values):
    if not values or any(value is None for value in values):
        return [None, None]
    ordered = sorted(values)
    return [
        value if math.isfinite(value) else None
        for value in (_percentile(ordered, 0.025), _percentile(ordered, 0.975))
    ]


def _safe(value):
    return value if value is not None and math.isfinite(value) else None


def _metric(totals, indices, name):
    selected = [totals[index] for index in indices]
    numerator_key, denominator_key = {
        "ite_per_completed_task": ("ite", "passes"),
        "usd_per_completed_task": ("usd", "passes"),
        "ite_per_task": ("ite", "runs"),
        "usd_per_task": ("usd", "runs"),
        "requests_per_task": ("requests", "runs"),
        "mean_context_tokens": ("input_tokens", "requests"),
        "output_tokens_per_task": ("output_tokens", "runs"),
        "wall_s_per_task": ("wall_s", "runs"),
    }[name]
    numerator = _sum_known([item[numerator_key] for item in selected])
    denominator = sum(item[denominator_key] for item in selected)
    if numerator is None:
        return None
    if denominator == 0:
        return math.inf if denominator_key == "passes" else None
    return numerator / denominator


def _arm_metrics(totals, draws):
    indices = range(len(totals))
    return {
        name: {
            "value": _safe(_metric(totals, indices, name)),
            "ci95": _ci([_metric(totals, draw, name) for draw in draws]),
        }
        for name in ARM_METRICS
    }


def _task_metric(totals, name):
    if name == "mean_context_tokens":
        numerator, denominator = totals["input_tokens"], totals["requests"]
    else:
        key = {"output_tokens": "output_tokens", "wall_s": "wall_s"}.get(name, name)
        numerator, denominator = totals[key], totals["runs"]
    return numerator / denominator if numerator is not None and denominator else None


def _ratio(tasks, a_totals, b_totals, draws, name):
    logs = []
    unavailable = []
    for task, arm_a, arm_b in zip(tasks, a_totals, b_totals, strict=True):
        a_value = _task_metric(arm_a, name)
        b_value = _task_metric(arm_b, name)
        if a_value is None or b_value is None or a_value <= 0 or b_value <= 0:
            unavailable.append(task)
        else:
            logs.append(math.log(b_value / a_value))
    if unavailable:
        return {"value": None, "ci95": [None, None], "unavailable_tasks": unavailable}
    value = math.exp(sum(logs) / len(logs))
    samples = [
        math.exp(sum(logs[index] for index in draw) / len(draw)) for draw in draws
    ]
    return {"value": value, "ci95": _ci(samples), "unavailable_tasks": []}


def _parse_fires(specifications):
    patterns = []
    names = set()
    for specification in specifications or []:
        if "=" not in specification:
            raise ValueError(f"--fire requires NAME=REGEX: {specification}")
        name, expression = specification.split("=", 1)
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", name) or name in names:
            raise ValueError(f"--fire names must be unique identifiers: {name}")
        try:
            pattern = re.compile(expression)
        except re.error as error:
            raise ValueError(f"invalid --fire regex {name}: {error}") from error
        patterns.append((name, expression, pattern))
        names.add(name)
    return patterns


def _feature_counts(rows, patterns):
    counts = {name: {"matches": 0, "requests": 0, "runs": 0} for name, _, _ in patterns}
    for row in rows:
        fired_in_run = set()
        for request in row.get("request_rows") or []:
            body_file = request.get("body_file")
            if not body_file:
                raise ValueError(
                    f"{row['_path']}: --fire requires saved request bodies"
                )
            body_path = Path(row["_path"]).parent / "proxy" / body_file
            text = gzip.decompress(body_path.read_bytes()).decode(
                "utf-8", errors="replace"
            )
            for name, _, pattern in patterns:
                matches = sum(1 for _ in pattern.finditer(text))
                counts[name]["matches"] += matches
                if matches:
                    counts[name]["requests"] += 1
                    fired_in_run.add(name)
        for name in fired_in_run:
            counts[name]["runs"] += 1
    return counts


def compare_runs(directory_a, directory_b, harness=None, margin=-8.0, fires=None):
    """Compare complete task/rep pairs; bootstrap whole tasks, preserving reps."""
    if not -100 <= margin <= 100:
        raise ValueError("--margin must be percentage points between -100 and 100")
    rows_a, harness_a, path_a = _read_arm(directory_a, harness)
    rows_b, harness_b, path_b = _read_arm(directory_b, harness)
    if _settings(rows_a) != _settings(rows_b):
        raise ValueError("arms must use the same model and reasoning effort")
    model, effort = _settings(rows_a)
    indexed_a, indexed_b = _index(rows_a), _index(rows_b)
    for task in {key[0] for key in indexed_a} | {key[0] for key in indexed_b}:
        keys_a = [key for key in indexed_a if key[0] == task]
        keys_b = [key for key in indexed_b if key[0] == task]
        if len(keys_a) == len(keys_b) == 1 and (
            keys_a[0][1] is None or keys_b[0][1] is None
        ):
            indexed_a[(task, None)] = indexed_a.pop(keys_a[0])
            indexed_b[(task, None)] = indexed_b.pop(keys_b[0])
    if indexed_a.keys() != indexed_b.keys():
        missing_a = sorted(
            indexed_b.keys() - indexed_a.keys(),
            key=lambda key: (key[0], -1 if key[1] is None else key[1]),
        )
        missing_b = sorted(
            indexed_a.keys() - indexed_b.keys(),
            key=lambda key: (key[0], -1 if key[1] is None else key[1]),
        )
        raise ValueError(
            f"unpaired task/rep runs (missing in A: {missing_a}; missing in B: {missing_b})"
        )
    by_task = defaultdict(list)
    for key in indexed_a:
        by_task[key[0]].append(key)
    tasks = sorted(by_task)
    a_totals, b_totals, flips = [], [], []
    for task in tasks:
        keys = sorted(by_task[task], key=lambda key: -1 if key[1] is None else key[1])
        paired = [(indexed_a[key], indexed_b[key]) for key in keys]
        a_totals.append(_task_totals([a for a, _ in paired]))
        b_totals.append(_task_totals([b for _, b in paired]))
        flips.append(
            {
                "task": task,
                "runs": len(paired),
                "a_passes": sum(a["pass"] for a, _ in paired),
                "b_passes": sum(b["pass"] for _, b in paired),
                "fail_to_pass": sum(not a["pass"] and b["pass"] for a, b in paired),
                "pass_to_fail": sum(a["pass"] and not b["pass"] for a, b in paired),
            }
        )
    rng = random.Random(BOOTSTRAP_SEED)
    draws = [
        tuple(rng.randrange(len(tasks)) for _ in tasks)
        for _ in range(BOOTSTRAP_RESAMPLES)
    ]
    task_deltas = [
        b["passes"] / b["runs"] - a["passes"] / a["runs"]
        for a, b in zip(a_totals, b_totals, strict=True)
    ]
    delta = 100 * sum(task_deltas) / len(tasks)
    delta_ci = _ci(
        [100 * sum(task_deltas[index] for index in draw) / len(draw) for draw in draws]
    )
    verdict = (
        "non-inferior"
        if delta_ci[0] >= margin
        else "inferior"
        if delta_ci[1] < margin
        else "inconclusive"
    )
    patterns = _parse_fires(fires)
    feature_a = _feature_counts(rows_a, patterns) if patterns else {}
    feature_b = _feature_counts(rows_b, patterns) if patterns else {}
    return {
        "mode": "comparison",
        "model": model,
        "effort": effort,
        "task_count": len(tasks),
        "paired_runs": len(indexed_a),
        "bootstrap_seed": BOOTSTRAP_SEED,
        "bootstrap_resamples": BOOTSTRAP_RESAMPLES,
        "margin_pp": margin,
        "arm_a": {
            "directory": str(path_a),
            "harness": harness_a,
            "runs": len(rows_a),
            "passes": sum(row["pass"] for row in rows_a),
            "pass_rate": sum(row["pass"] for row in rows_a) / len(rows_a),
            "pass_rate_ci95": _wilson(sum(row["pass"] for row in rows_a), len(rows_a)),
            "metrics": _arm_metrics(a_totals, draws),
        },
        "arm_b": {
            "directory": str(path_b),
            "harness": harness_b,
            "runs": len(rows_b),
            "passes": sum(row["pass"] for row in rows_b),
            "pass_rate": sum(row["pass"] for row in rows_b) / len(rows_b),
            "pass_rate_ci95": _wilson(sum(row["pass"] for row in rows_b), len(rows_b)),
            "metrics": _arm_metrics(b_totals, draws),
        },
        "paired": {
            "delta_success_pp": {"value": delta, "ci95": delta_ci, "verdict": verdict},
            "geometric_ratios_b_over_a": {
                name: _ratio(tasks, a_totals, b_totals, draws, name)
                for name in RATIO_METRICS
            },
        },
        "flips": flips,
        "features": [
            {
                "name": name,
                "regex": expression,
                "a": feature_a[name],
                "b": feature_b[name],
            }
            for name, expression, _ in patterns
        ],
    }


def _fmt(value, digits=2):
    return "n/a" if value is None else f"{value:,.{digits}f}"


def _interval(interval, digits=2):
    if interval == [None, None]:
        return "n/a"
    upper = "unbounded" if interval[1] is None else _fmt(interval[1], digits)
    return f"[{_fmt(interval[0], digits)}, {upper}]"


def markdown(report):
    a, b = report["arm_a"], report["arm_b"]
    delta = report["paired"]["delta_success_pp"]
    lines = [
        "# Paired harness comparison",
        "",
        f"A: `{a['directory']}` ({a['harness']}); B: `{b['directory']}` ({b['harness']}).",
        f"Model `{report['model']}`, effort `{report['effort']}`; {report['task_count']} tasks, {report['paired_runs']} paired runs; {report['bootstrap_resamples']:,} task bootstrap resamples, seed {report['bootstrap_seed']}.",
        "Model and effort are checked from run records. Keep timeout and upstream fixed in the run setup; those settings are not in result.json.",
        "",
        "## Success gate",
        "",
        "| Arm | Passes | Pass rate | Wilson 95% CI |",
        "| --- | ---: | ---: | ---: |",
    ]
    for label, arm in (("A", a), ("B", b)):
        lines.append(
            f"| {label} | {arm['passes']}/{arm['runs']} | {arm['pass_rate']:.1%} | {_interval([100 * x for x in arm['pass_rate_ci95']], 1)} pp |"
        )
    lines += [
        "",
        f"Paired Δsuccess (B − A): **{_fmt(delta['value'], 1)} pp**; task-bootstrap 95% CI {_interval(delta['ci95'], 1)} pp. Non-inferiority margin: {_fmt(report['margin_pp'], 1)} pp. Verdict: **{delta['verdict']}**.",
        "",
        "## Arm metrics",
        "",
        "ITE and USD per completed task include every run, including failures and timeouts, in the numerator. A zero-pass bootstrap sample has an unbounded upper cost.",
        "",
        "| Metric | A (95% task-bootstrap CI) | B (95% task-bootstrap CI) |",
        "| --- | ---: | ---: |",
    ]
    labels = {
        "ite_per_completed_task": "ITE/completed task",
        "usd_per_completed_task": "USD/completed task",
        "ite_per_task": "ITE/task",
        "usd_per_task": "USD/task",
        "requests_per_task": "Requests/task",
        "mean_context_tokens": "Mean context tokens/request",
        "output_tokens_per_task": "Output tokens/task",
        "wall_s_per_task": "Wall seconds/task",
    }
    for name in ARM_METRICS:
        digits = 6 if name.startswith("usd") else 2
        left, right = a["metrics"][name], b["metrics"][name]
        lines.append(
            f"| {labels[name]} | {_fmt(left['value'], digits)} {_interval(left['ci95'], digits)} | {_fmt(right['value'], digits)} {_interval(right['ci95'], digits)} |"
        )
    lines += [
        "",
        "## Paired per-task geometric ratios (B/A)",
        "",
        "Each task contributes one log ratio of its arm means; all reps and failures are included. A ratio below 1 means B used less or finished faster.",
        "",
        "| Metric | Ratio | 95% task-bootstrap CI | Unavailable tasks |",
        "| --- | ---: | ---: | --- |",
    ]
    for name, result in report["paired"]["geometric_ratios_b_over_a"].items():
        lines.append(
            f"| {name} | {_fmt(result['value'], 3)} | {_interval(result['ci95'], 3)} | {', '.join(result['unavailable_tasks']) or '—'} |"
        )
    lines += [
        "",
        "## Per-task flips",
        "",
        "| Task | A passes | B passes | Fail→pass | Pass→fail |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for flip in report["flips"]:
        lines.append(
            f"| {flip['task']} | {flip['a_passes']}/{flip['runs']} | {flip['b_passes']}/{flip['runs']} | {flip['fail_to_pass']} | {flip['pass_to_fail']} |"
        )
    if report["features"]:
        lines += [
            "",
            "## Feature firing",
            "",
            "Regex matches are counted in the decompressed UTF-8 request JSON; request and run counts record where each pattern appeared.",
            "",
            "| Feature | A matches / requests / runs | B matches / requests / runs |",
            "| --- | ---: | ---: |",
        ]
        for feature in report["features"]:
            left, right = feature["a"], feature["b"]
            lines.append(
                f"| {feature['name']} | {left['matches']} / {left['requests']} / {left['runs']} | {right['matches']} / {right['requests']} / {right['runs']} |"
            )
    return "\n".join(lines) + "\n"
