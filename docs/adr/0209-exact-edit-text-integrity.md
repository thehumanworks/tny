# ADR 0209: Exact-edit text integrity

Status: accepted

## Goal

An exact text edit must never report success after silently discarding file bytes.

## Decision

The shared exact-edit helper rejects any source containing NUL before substring
matching, write hooks, undo recording, or replacement. It returns the appended
`TNY_EDIT_BINARY` status; CLI and native tools report that exact edits require
NUL-free text. Existing status values and ordinary text matching remain unchanged.
This is a text contract, not a binary editor or a new filesystem lock protocol.

Built-in `edit_file` also rejects NUL in decoded path, search, and replacement
arguments before path resolution or permission detail construction. Custom tool
overrides keep their own contracts. CLI JSON and marker payload validation, and
code-mode nested-argument validation, already rejected these arguments and remain
unchanged.

The shared guard also protects intercepted SSH `tny edit` commands after staging.
Direct SSH `edit_file` already rejects NUL-bearing content. The helper introduces
no native-only dependency; wasm retains its existing execution support boundary.

## Verification

The edit unit test places NUL at every source-byte position in both replacement
modes and checks unchanged bytes, zero matches, and zero write-hook calls. Native
dispatch tests verify byte identity and preservation of existing undo metadata and
blob content for binary files and NUL-bearing arguments. Production CLI tests cover
source and payload rejection; synthetic Responses and Chat Completions tests cover
real nested `edit_file` calls without live inference. Existing ordinary-text tests
remain the compatibility checks. The SSH interception fixture verifies that a
NUL-bearing remote source is not streamed back; its ordinary-text setup uses the
actual string length rather than accidentally writing the C terminator.

The source-file regression fixtures fail on the pre-change production binary:
four CLI cases and both provider-wire cases report success and discard bytes;
two leading-NUL CLI cases misleadingly report no match. The candidate must reject
all eight without writing. Argument fixtures exercise existing wire validation
and the newly guarded direct native admission, not a newly discovered wire bug.
