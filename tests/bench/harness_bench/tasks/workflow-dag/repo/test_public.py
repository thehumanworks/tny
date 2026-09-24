from workflow import simulate

r = simulate(
    [{"id": "a", "duration": 2}, {"id": "b", "duration": 1, "depends_on": ["a"]}]
)
assert r["makespan"] == 3
assert r["tasks"]["b"] == {"state": "succeeded", "attempts": 1, "finish": 3}
