from records import dedupe_records

assert dedupe_records([" Alice ", "alice", "Bob", "BOB", "carol"]) == [
    " Alice ",
    "Bob",
    "carol",
]
