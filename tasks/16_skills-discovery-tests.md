# 16 — Test SKILL.md discovery, load, and install

High. From the test-depth review. Independent of 15/17–19.

`src/core/skills.c` implements the documented search order (`skills/`,
`.agents/skills/`, `.claude/…`, user `~/.tny/skills/`, stop before
`$HOME`). `tests/` never calls `skills_discover` / `skills_load` /
`skills_install`. The only `SKILL.md` write is in
`tests/integration/test_libtny.py` to prove **embedded** runtimes do
**not** leak HOME skill metadata. Product `skill` / `install_skill`
tools have no happy path.

## Work

- Add a fixture tree (temp workspace + fake `$HOME` / `TNY_HOME`) with
  `SKILL.md` files at each documented root, including a HOME-ancestor
  directory that must **not** contribute skills.
- Unit-test `skills_discover`: nearer root wins on name collision,
  frontmatter `name`/`description` parse, missing `---` / missing
  `name:` rejected, body not loaded at discover time.
- Unit-test `skills_load` (lazy body) and `skills_install` (copy only
  into `~/.tny/skills/<name>`, error buffer on failure).
- Drive the native `skill` and `install_skill` tools through
  `tools_call_prepare` / execute (not only the C helpers) for one happy
  path and one collision.
- Keep the libtny “HOME metadata must not leak into embeddings” case;
  do not replace it.

## Acceptance

- A skill placed in `$HOME/skills/` (parent of the workspace) is absent
  from `skills_discover` for that workspace.
- A valid workspace `skills/foo/SKILL.md` is listed, loadable, and
  installable into the fake `~/.tny/skills/foo`.
- `make test` fails if search order, frontmatter, or install destination
  regresses.
