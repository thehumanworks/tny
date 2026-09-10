#!/usr/bin/env python3
"""Decide the next release tag for a commit on main (docs/adr/0085).

The tag is the single source of truth for the version (docs/adr/0014), so
releasing means choosing the next tag. This script reads the stable `vX.Y.Z`
tags reachable from the commit, classifies every commit since the newest one
by its Conventional Commits prefix, and prints the tag that
`.github/workflows/auto-release.yml` should create:

    feat:             minor bump
    feat!: / BREAKING minor bump below 1.0.0, major bump from 1.0.0 on
    anything else     patch bump

It prints nothing and exits 0 with a reason on stderr when there is nothing
to release: the commit already carries a release tag, a newer release exists
elsewhere on main (a stale CI completion), or the commits the head introduced
carry `[skip release]`. It exits 1 on git errors. Pre-release tags (`v1.2.3-rc.1`) are never bases and
never bumped from; humans cut those by hand.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

STABLE_TAG = re.compile(r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
# `type!: subject`, `type(scope)!: subject` (Conventional Commits 1.0.0).
BREAKING_SUBJECT = re.compile(r"^[A-Za-z]+(?:\([^)]*\))?!:")
BREAKING_FOOTER = re.compile(r"^BREAKING[ -]CHANGE:", re.MULTILINE)
FEATURE_SUBJECT = re.compile(r"^feat(?:\([^)]*\))?!?:")
SKIP_RELEASE = re.compile(
    r"\[(?:skip release|release skip|no release)\]", re.IGNORECASE
)

PATCH, MINOR, MAJOR = "patch", "minor", "major"
BUMP_RANK = {PATCH: 0, MINOR: 1, MAJOR: 2}


@dataclass(frozen=True, order=True)
class Version:
    major: int
    minor: int
    patch: int

    @property
    def tag(self) -> str:
        return f"v{self.major}.{self.minor}.{self.patch}"


def parse_stable_tag(tag: str) -> Version | None:
    """Return the version of a stable `vX.Y.Z` tag, or None for anything else."""
    match = STABLE_TAG.match(tag.strip())
    if match is None:
        return None
    return Version(*(int(match.group(index)) for index in range(1, 4)))


def highest_stable(tags: Iterable[str]) -> Version | None:
    versions = [version for version in map(parse_stable_tag, tags) if version]
    return max(versions) if versions else None


def bump_kind_for_message(message: str) -> str:
    """Classify one commit message (subject + body) into a bump kind."""
    subject = message.lstrip().split("\n", 1)[0]
    if BREAKING_SUBJECT.match(subject) or BREAKING_FOOTER.search(message):
        return MAJOR
    if FEATURE_SUBJECT.match(subject):
        return MINOR
    return PATCH


def bump_kind_for_messages(messages: Iterable[str]) -> str:
    kind = PATCH
    for message in messages:
        candidate = bump_kind_for_message(message)
        if BUMP_RANK[candidate] > BUMP_RANK[kind]:
            kind = candidate
    return kind


def bump(base: Version, kind: str) -> Version:
    """Apply a bump; breaking changes only bump the minor before 1.0.0."""
    if kind == MAJOR and base.major == 0:
        kind = MINOR
    if kind == MAJOR:
        return Version(base.major + 1, 0, 0)
    if kind == MINOR:
        return Version(base.major, base.minor + 1, 0)
    if kind == PATCH:
        return Version(base.major, base.minor, base.patch + 1)
    raise ValueError(f"unknown bump kind {kind!r}")


class Repo:
    def __init__(self, path: Path) -> None:
        self.path = path

    def git(self, *args: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(self.path), *args],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout

    def tags(self, *filters: str) -> list[str]:
        return self.git("tag", "--list", "v*", *filters).split()

    def rev_parse(self, ref: str) -> str:
        return self.git("rev-parse", "--verify", f"{ref}^{{commit}}").strip()

    def messages(self, range_spec: str) -> list[str]:
        raw = self.git("log", "--format=%B%x00", range_spec)
        return [message for message in raw.split("\0") if message.strip()]

    def is_merge(self, commit: str) -> bool:
        parents = self.git("rev-list", "--parents", "-n", "1", commit).split()
        return len(parents) > 2


@dataclass(frozen=True)
class Plan:
    tag: str | None
    reason: str
    bump: str | None = None
    base: str | None = None


def plan_release(repo: Repo, commit: str, override: str | None = None) -> Plan:
    """Decide whether `commit` gets a new tag, and which one."""
    commit = repo.rev_parse(commit)
    base = highest_stable(repo.tags("--merged", commit))
    newest = highest_stable(repo.tags())
    if newest is not None and (base is None or newest > base):
        return Plan(
            None,
            f"{newest.tag} is newer than every release reachable from {commit[:12]}",
        )
    if base is not None and repo.rev_parse(base.tag) == commit:
        return Plan(
            None, f"{commit[:12]} is already released as {base.tag}", base=base.tag
        )

    introduced = f"{commit}^1..{commit}" if repo.is_merge(commit) else f"{commit}^!"
    if any(SKIP_RELEASE.search(message) for message in repo.messages(introduced)):
        return Plan(
            None,
            f"{commit[:12]} opts out with [skip release]",
            base=base.tag if base else None,
        )

    since = f"{base.tag}..{commit}" if base is not None else commit
    messages = repo.messages(since)
    if not messages:
        return Plan(
            None,
            f"no commits since {base.tag if base else 'the root'}",
            base=base.tag if base else None,
        )
    kind = override or bump_kind_for_messages(messages)
    # Any existing stable tag above `base` already returned above, so the
    # computed tag is new locally; the tag push rejects a remote collision.
    version = bump(base or Version(0, 0, 0), kind)
    return Plan(
        version.tag,
        f"{len(messages)} commit(s) since {base.tag if base else 'the root'}",
        kind,
        base.tag if base else None,
    )


def write_outputs(path: Path | None, plan: Plan) -> None:
    if path is None:
        return
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"tag={plan.tag or ''}\n")
        stream.write(f"bump={plan.bump or ''}\n")
        stream.write(f"base={plan.base or ''}\n")
        stream.write(f"reason={plan.reason}\n")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--commit", default="HEAD")
    parser.add_argument(
        "--bump",
        choices=[PATCH, MINOR, MAJOR],
        help="force the bump kind instead of reading commit messages",
    )
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args(argv)
    try:
        plan = plan_release(Repo(args.repo), args.commit, args.bump)
    except subprocess.CalledProcessError as error:
        detail = getattr(error, "stderr", None) or str(error)
        print(f"error: {detail.strip()}", file=sys.stderr)
        return 1
    write_outputs(args.github_output, plan)
    if plan.tag is None:
        print(f"skip: {plan.reason}", file=sys.stderr)
    else:
        print(
            f"{plan.tag} ({plan.bump} bump from {plan.base or 'no release'}: {plan.reason})",
            file=sys.stderr,
        )
        print(plan.tag)
    return 0


if __name__ == "__main__":
    sys.exit(main())
