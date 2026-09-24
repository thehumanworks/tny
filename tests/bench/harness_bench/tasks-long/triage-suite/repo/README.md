# Commerce operations toolkit

Twenty small modules support order intake, billing, routing, and reporting.
The release regression suite is under `tests/`. Run it with:

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

The tests describe the public behavior. Each failure is a separate defect;
fix the implementation rather than changing test expectations.
