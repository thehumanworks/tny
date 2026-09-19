# Schemas

JSON Schema Draft 2020-12, version 1. See `../docs/api.md` for semantic constraints
that JSON Schema cannot express (UTF-8 byte limits, current ownership/revisions,
column references, filesystem safety, history alignment and dispatch fencing).
Relative `$ref` links resolve within this directory. No schema fetching occurs
at runtime. Unknown object properties are rejected.
