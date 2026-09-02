# 0064 — CLI verb conventions: stdin payloads and stable results

Date: 2026-09-02
Status: accepted (implements the first-party verb direction in ADR 0057)

## Context

ADR 0057 makes small first-party `tny` verbs available through an ordinary
shell, both inside tny and from other coding harnesses. These commands are a
public automation interface. If each invents its own argv payload, output
shape, diagnostics, or exit status, callers must quote large and possibly
secret content into process listings and cannot compose the verbs reliably.

The first verb, `tny edit` (issue #96), carries multiline exact-match text and
must distinguish usage or I/O failure from the semantic result "the requested
match is not unique." Later verbs in issues #97 and #98 need the same channel
and status conventions.

## Decision

1. **Payload is stdin-only.** Large, multiline, structured, or sensitive verb
   payloads never ride argv. Argv selects the verb, target, and small control
   flags only.
2. **The human stdin form is a line fence.** Its default prefix is `***`, with
   exact delimiter lines `*** SEARCH`, `*** REPLACE`, and `*** END`. A verb
   that accepts fenced text provides `--marker STR`, producing the same lines
   with `STR` as their prefix. One structural line ending immediately before a
   delimiter is excluded from the preceding body; a blank line represents a
   body that ends in a newline.
3. **`--json` is a complete machine contract.** Where a verb has a structured
   input form, `--json` selects JSON on stdin as well as JSON on stdout. A
   successful invocation prints exactly one JSON object. Every object has a
   stable `kind` string; edit uses `kind:"edit"` plus `path`, `matches`, and
   `replaced`. No progress text is mixed into stdout.
4. **stderr is operational.** Progress, warnings, context hints, and errors go
   to stderr in both human and JSON modes. A semantic failure prints the facts
   needed to retry safely; edit prints the match count and, when zero, a unique
   nearest context line when one can be identified.
5. **Exit statuses are shared.** Verbs use the following table unless a later
   ADR explicitly adds a command-specific status:

   | Exit | Meaning |
   | ---: | --- |
   | 0 | Operation completed successfully |
   | 1 | Usage, configuration, input, allocation, or I/O failure |
   | 2 | Well-formed request could not be applied semantically |
   | 130 | Interrupted |

6. **Failure is atomic.** A verb does not expose a partially applied result.
   `tny edit` counts first, constructs the full output, then uses the shared
   interruptible temp-file-and-rename path. It resolves an existing symlink and
   edits its target rather than replacing the link.
7. **Stateless verbs bypass configuration.** Help and execution for a verb that
   needs no runtime context are dispatched before settings load. `tny edit`
   uses the current process directory for relative paths, accepts absolute
   paths, and does not implement `--ssh`; remote interception belongs to issue
   #99.
8. **wasm behavior is explicit.** `tny edit` works on the same virtual
   filesystem as the built-in file tool. Future verbs must document whether
   they work, are remote-only, or return a clean unsupported error.

## Consequences

- Shell scripts and other harnesses can choose readable fences or one-object
  JSON without placing the replacement payload in shell history or `ps`.
- Stdout remains pipe-safe, while stderr remains useful to a human or agent
  diagnosing a retry.
- Exit 2 is a stable semantic branch rather than an undifferentiated runtime
  error; callers can widen an exact match without retrying I/O failures.
- New verbs have a compatibility obligation for delimiter parsing, `kind`
  values, fields, and exit statuses. Any incompatible change needs a new ADR.
