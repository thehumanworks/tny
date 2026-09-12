# 0091 — Local strict-image failure detail without generic diagnostic leakage

Date: 2026-09-11
Status: proposed; implementation and verification pending
Requirement: R122.1–R122.4, I122.1–I122.4; canonical contract ../verification/open-issues-2026-09-11/contract.md, A9.

## Decision

Expose a narrow structured detail for exactly the four locally assigned strict-size rejection codes, using the existing result/error channels. This is not arbitrary provider diagnostic serialization. Retain error exit/status, native-tool error prefix, generic SDK error message, and no-output-committed state. Only explicit JSON interception returns detail on stdout. Generic provider/cancel/OOM errors have no result; final cancel/OOM clears staged detail. Result structs are initialized before fallible work.

The native toolkit's private job state marks locally constructed detail and the existing result accessor may return it after failed completion. No public ABI record, function, version or successful result changes. Python copies before destroy and attaches optional read-only image_detail to the same typed generic exception. TypeScript does likewise with a non-enumerable read-only imageDetail property and distinct validated failure type. Default formatting never prints the detail or any provider/configuration value. Callers may deliberately inspect the narrow object.

## Alternatives and consequences

Discarding all failure detail was simple but violates structured strict-failure parity. Copying arbitrary provider error text or request dumps would leak secrets. A new ABI entry point or error hierarchy is unnecessary when the existing completed-result channel suffices. The narrow semantic extension requires updating the strict-size ABI test and the errors-have-no-result documentation while retaining generic-failure emptiness. Non-JSON CLI/interception behavior remains compatible. Metadata allocation failure degrades to a generic failure rather than invented success.

## Validation

Read-only design reviewer 1dde9053-32bb-4b8d-b236-b5504b013e1c conditionally approved; all six exact call-site/type/lifetime corrections are mandatory in A9 before implementation. Real loopback tests must cover all callers, preflight versus paid mismatch counts, output hash preservation, safe metadata/no commit, generic and cancelled failures, fake secret sentinels and normal error formatting. ABI inventories, SDK type checks and memory/fault tests remain required. A fresh actual-code review is required before manifest reuse. No implementation or runtime success is established by this proposed ADR.
