from jdiff import diff_values

assert diff_values({"a": 1}, {"a": 2}) == [
    {"path": "/a", "kind": "changed", "before": 1, "after": 2}
]
