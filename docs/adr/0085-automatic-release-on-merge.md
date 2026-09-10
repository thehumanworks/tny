# 0085 — Every green merge to main is tagged and published automatically

Date: 2026-09-09
Status: accepted

## Context

ADR 0014 made the git tag the single source of truth for the version, so a
release was "merge, then `git tag v<version> && git push`". That still left
two manual steps and two ways to get them wrong: forgetting to tag at all
(v0.8.0 shipped nine commits after v0.7.0 only because someone remembered),
and tagging the wrong commit — the Pages workflow follows most merges with
a `[skip ci]` mirror commit, and a tag on that commit never starts
`release.yml`, which is why v0.7.0 needed a dedicated `release:` commit.
Choosing the number was also ad hoc: `v0.6.0` → `v0.7.0` → `v0.8.0` were
all minor bumps regardless of content.

GitHub Actions adds one constraint: a tag pushed with the workflow's own
`GITHUB_TOKEN` does not trigger `on: push: tags` workflows. `release.yml`
already carries the documented escape hatch, `workflow_dispatch` on the tag
ref, because `workflow_dispatch` is exempt from that rule.

## Decision

`.github/workflows/auto-release.yml` releases every merge to `main` that
passes every main-branch gate.

- **Trigger.** `workflow_run` on completion of `ci`, `nix`, and `sdk` for
  pushes to `main`. Each completion re-checks the other two through the
  Actions API (`/actions/runs?head_sha=`), so whichever finishes last
  tags; the earlier ones exit. A `concurrency` group serializes runs and
  never cancels, so two gates finishing together cannot lose a release.
- **Version.** `scripts/next_release_version.py` takes the highest stable
  `vX.Y.Z` tag reachable from the commit and bumps it from the
  Conventional Commits prefixes of every commit since it: `feat` → minor,
  `!` or `BREAKING CHANGE:` → major (minor while the major is 0), anything
  else → patch. Merge commits and un-prefixed messages count as patch, so a
  PR whose branch commits say `feat:` gets a minor bump even though its
  merge commit does not. Pre-release tags are ignored as bases; they stay
  hand-cut. No file records the version (ADR 0014 holds).
- **Never backwards.** If any stable tag is newer than every tag reachable
  from the commit, the commit is stale (its gates finished after a later
  merge was released) and is skipped. A commit that already carries the
  newest tag is skipped too, so re-runs are idempotent.
- **Opt-out.** `[skip release]` / `[no release]` in the squashed commit or
  in any commit the merge introduces skips that merge; the next merge picks
  everything up. The marker is checked only on the commits the head
  introduced, never on the whole range, so one opt-out cannot block later
  merges.
- **Publish.** The job pushes an annotated tag as `github-actions[bot]`
  (`contents: write`), then dispatches `release.yml` on the tag ref
  (`actions: write`) and waits until that run exists, failing visibly if
  it does not so a bare tag is never silent. `release.yml` is unchanged: it
  still builds, tests, size-gates, attests, and publishes from
  `GITHUB_REF_NAME`, which a dispatch on a tag ref sets to the tag.
- **Manual.** `workflow_dispatch` on `auto-release` releases the newest
  commit on `main` whose gates are green (walking back over `[skip ci]`
  mirror commits), with an optional forced bump kind. Hand-pushed tags
  keep working through the existing tag trigger.

## Consequences

- Releasing is zero steps. Every merge to `main` that is green everywhere
  becomes a GitHub release, with mise-resolvable assets, within one release
  workflow run of the last gate finishing.
- Version numbers follow commit prefixes, so PR authors choose the bump by
  writing `feat:`, `fix:`, or `feat!:`. Un-prefixed commits are safe (patch)
  but invisible in the number; the repo's existing habit of prefixed
  subjects is now load-bearing.
- A merge that only touches CI or docs still cuts a patch release. This is
  accepted over a path filter: a release is cheap, and "was that change
  released?" always has the same answer. Use `[skip release]` for the rare
  merge that must not ship.
- Gates that are red on `main` block releases until they are green again;
  re-running the failed workflow to success re-triggers `auto-release`
  through the same `workflow_run` completion event.
- If a ruleset ever protects `v*` tags from `github-actions[bot]`, the tag
  push fails in the `auto-release` run, which is the visible signal to grant
  it (or to pass a token that may push tags).
- `tests/packaging/test_next_release_version.py` covers the version
  arithmetic against throwaway git repositories and pins the workflow
  contract (all three gates, the dispatch on the tag ref); it runs in the
  `sdk` workflow with the other packaging tests.
