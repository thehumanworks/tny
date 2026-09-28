import re

_ID = re.compile(r"[A-Za-z][A-Za-z0-9_-]{0,31}\Z")


def _bounded_int(value, low, high):
    return type(value) is int and low <= value <= high


def simulate(tasks, workers=2, retries=1):
    if (
        not isinstance(tasks, list)
        or len(tasks) > 32
        or not _bounded_int(workers, 1, 8)
        or not _bounded_int(retries, 0, 8)
    ):
        raise ValueError("invalid input")
    specs = {}
    for item in tasks:
        if (
            not isinstance(item, dict)
            or not {"id", "duration"} <= item.keys()
            or item.keys() - {"id", "duration", "depends_on", "failures"}
        ):
            raise ValueError("task shape")
        name = item["id"]
        deps = item.get("depends_on", [])
        if (
            not isinstance(name, str)
            or not _ID.fullmatch(name)
            or name in specs
            or not _bounded_int(item["duration"], 1, 1000)
            or not _bounded_int(item.get("failures", 0), 0, 8)
        ):
            raise ValueError("task fields")
        if (
            not isinstance(deps, list)
            or len(set(map(str, deps))) != len(deps)
            or any(not isinstance(d, str) for d in deps)
        ):
            raise ValueError("dependencies")
        specs[name] = dict(
            duration=item["duration"],
            depends_on=deps[:],
            failures=item.get("failures", 0),
        )
    for name, spec in specs.items():
        if any(dep not in specs or dep == name for dep in spec["depends_on"]):
            raise ValueError("missing or self dependency")
    marks = {}

    def visit(name):
        if marks.get(name) == 1:
            raise ValueError("cycle")
        if marks.get(name) == 2:
            return
        marks[name] = 1
        for dep in specs[name]["depends_on"]:
            visit(dep)
        marks[name] = 2

    for name in specs:
        visit(name)
    states = {name: "pending" for name in specs}
    attempts = dict.fromkeys(specs, 0)
    finish = {}
    active = {}
    events = []
    now = 0

    def event(name, kind):
        events.append(dict(time=now, task=name, attempt=attempts[name], type=kind))

    while True:
        for name in sorted(name for name, end in active.items() if end == now):
            del active[name]
            if attempts[name] <= specs[name]["failures"]:
                states[name] = "pending" if attempts[name] <= retries else "failed"
                event(name, "retry" if states[name] == "pending" else "failed")
            else:
                states[name] = "succeeded"
                event(name, "succeeded")
            if states[name] != "pending":
                finish[name] = now
        while True:
            skips = sorted(
                name
                for name in specs
                if states[name] == "pending"
                and any(
                    states[d] in ("failed", "skipped")
                    for d in specs[name]["depends_on"]
                )
            )
            if not skips:
                break
            for name in skips:
                states[name] = "skipped"
                finish[name] = now
                event(name, "skipped")
        for name in sorted(specs):
            if len(active) >= workers:
                break
            if states[name] == "pending" and all(
                states[d] == "succeeded" for d in specs[name]["depends_on"]
            ):
                attempts[name] += 1
                states[name] = "running"
                active[name] = now + specs[name]["duration"]
                event(name, "start")
        if all(
            state in ("succeeded", "failed", "skipped") for state in states.values()
        ):
            return dict(
                makespan=now,
                tasks={
                    name: dict(
                        state=states[name], attempts=attempts[name], finish=finish[name]
                    )
                    for name in specs
                },
                events=events,
            )
        now = min(active.values())
