Implement ledger.py and ledger_cli.py using only Python's standard library.
Ledger(path) stores an inventory ledger durably at a file path; SQLite is permitted.
Implement apply(event_id, sku, delta), snapshot(), close(), and context-manager
support. event_id and sku must be nonempty strings of at most 64 characters;
delta must be an int excluding bool. Invalid inputs raise ValueError, with no
state change. Stock starts at zero and must never go negative: reject an invalid
adjustment with ValueError without consuming its event_id. Valid apply returns
the resulting stock for that event's sku. Repeating an event_id with identical sku
and delta returns the ORIGINAL event result, even after later changes; a different
sku or delta for the same ID raises ValueError without mutation. snapshot returns
a dict of all successfully touched SKUs, including zero balances. State persists
across close/reopen. Independent Ledger connections, including simultaneous
threads with one connection per thread, must not lose updates or admit duplicates.
An error must leave the connection usable for a later valid operation.

python ledger_cli.py PATH reads JSON objects, one per stdin line, with exactly
keys event_id, sku, delta. For each valid line print one JSON object {"stock": N}.
For each invalid line (malformed JSON/shape/values or failed adjustment), print
{"error":"ValueError"}, continue subsequent lines, and eventually exit 2 if ANY
line failed, otherwise 0. No extra stdout. Empty input exits 0. Do not change the
public test. Add and run your own tests, including persistence and concurrency.
