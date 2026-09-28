# Record access

Call `RecordStore.resolve(scope, identifier, *, as_of=None)` to get a `Resolution`.
Check `found` before reading `value`; a stored null payload is valid. The
`revision` and `reason` fields explain absent, deleted and expired outcomes.
A historical read selects the greatest record revision at or before `as_of`;
a tombstone or expiry masks older values.

All client readers forward `as_of` and apply a fallback only for an unresolved
record. Active-record calls and stable key names preserve prior behavior.
