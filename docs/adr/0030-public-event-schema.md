# 0030 — One canonical public event schema for libtny and language SDKs

Date: 2026-08-26
Status: accepted

## Context

`tny_backend_event`, the libtny getter surface, extension-hook JSON, CLI JSON,
and future TypeScript/Python bindings all describe overlapping event concepts.
Allowing each adapter to spell that contract independently would make the SDK
surface drift from the engine and would force bindings to learn private C
layouts or human-oriented rendering fields.

## Decision

The checked-in registry at `sdk/schema/events.json` is the canonical public
runtime event vocabulary. It assigns stable numeric ids and payload field names
for the 14 events currently exposed by libtny. The extension-hook contract may
remain a lifecycle superset, but overlapping event names and data come from the
same normalized engine events.

Every public event has a common envelope:

- schema version;
- monotonically increasing session-local sequence;
- monotonic timestamp in milliseconds;
- provider name;
- tny session id;
- tny turn id;
- event type.

ABI 0.3 adds `tny_event_view_v0` and `tny_event_read`. The view is an append-only
sized structure containing fixed-width scalar values and borrowed `tny_bytes`
views. Its byte/string members have exactly the same lifetime as the event and
become invalid at `tny_event_free`. Existing field getter functions remain
exported for source and binary compatibility.

`message_id`, context usage/size, and cost are now available through the view
when the provider supplies them. Tool argument/result structure is deliberately
not added by this ADR; that is a separate bounded-data contract (#64).

The schema generator writes committed TypeScript and Python artifacts plus a
golden fixture. `python3 sdk/schema/check.py` verifies that the generated files
are current and that public C event constants match the registry. Normal tny
builds do not require TypeScript tooling or runtime code generation.

## Compatibility

- Event numeric ids and existing field meanings are stable within the public
  schema major.
- Adding optional fields is compatible.
- Consumers must ignore fields they do not understand.
- Language SDKs must retain an `unknown` representation for future event kinds
  rather than rejecting the stream.
- The generated `UnknownEvent` uses the reserved `type: "unknown"` discriminant,
  retains the unknown numeric `kind`, and keeps any provider-neutral original
  type separately. It must not use `type: string`, which overlaps every known
  literal and defeats TypeScript exhaustiveness checks.
- A schema-major change requires an explicit compatibility decision and new
  conformance fixtures.
- `tny_event_view_v0.struct_size` is caller supplied. Older/smaller supported
  views receive only their prefix; too-small views fail before writing data.

## Consequences

Language SDKs can generate discriminated event types without duplicating the
agent runtime or binding private structs. The C ABI has one efficient snapshot
operation while legacy getters remain usable. CLI JSONL event streaming can
reuse the same registry later (#66).

## Verification

- `sdk/schema/check.py` is part of `make test`.
- `tests/integration/test_libtny.py` verifies current view size, undersized-view
  rejection, schema version, kind parity, monotonic sequence/timestamps, and
  provider/session/turn metadata through the shared library.
- The libtny exact export allowlist includes the two new view functions.
- Existing C, C++, ctypes, CLI, TUI, provider, wasm-source and extension tests
  remain the regression gate.
