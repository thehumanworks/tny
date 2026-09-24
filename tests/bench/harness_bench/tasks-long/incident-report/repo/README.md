# Gateway saturation incident

Run `python3 -m unittest discover -s tests -v` for the service tests. The
`setup.sh` fixture creates `evidence/requests-*.jsonl` and
`evidence/metrics-*.csv`. Request IDs increase in event time order. Logs are
sharded by tenant; inspect all shards. Timestamps are UTC. The metric files
record a sampled pool view and can help locate the saturation window.

The incident is a burst on 2026-06-14. Inspect both service code and evidence.
Add `ANSWERS.json` at the repository root with exactly these fields:

- `root_cause`: short string naming the erroneous config precedence and impact.
- `first_failing_request`: request ID of the earliest 503.
- `first_failure_utc`: ISO timestamp of that request, with `Z`.
- `affected_tenants`: number of distinct tenants with a 503.
- `peak_5m_start_utc`: start of the earliest five-minute UTC window with the
  most 503s; use minute-aligned windows.
- `total_503`: count of all 503 responses in the incident logs.

The service has a default pool capacity and a premium tier override. A premium
request must use the override. Keep the public `pool_capacity_for` function.
