# Commerce operations toolkit

Twenty small modules support order intake, billing, routing, and reporting.
The release regression suite is under `tests/`. Run it with:

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

The tests describe the public behavior. Each failure is a separate defect;
fix the implementation rather than changing test expectations.

This is one release session with follow-up requests across modules. Setup
creates `fixtures/settlements.csv` and three JSONL traces in `evidence/`.
The trace rows identify boundary cases across the same operations covered by
the suite. Keep `RELEASE_NOTES.md` at the workspace root for the final release
summary.
