# Tenant record migration

All 30 client readers use `RecordStore.resolve` and forward historical
`as_of` revisions. An explicit null payload remains found. A tombstone masks
earlier versions; expiry applies at the query revision. A fallback is used
only for an absent, deleted, or expired resolution. The old API has been
removed from code and API documentation. The full visible suite passes.
