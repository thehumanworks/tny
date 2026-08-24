#!/usr/bin/env bash
# Install tny from source into ~/.local/bin.
# Read this file before piping it to a shell.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
REPO="${TNY_REPO:-https://github.com/thehumanworks/tny.git}"
REF="${TNY_REF:-}"

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'tny setup: missing %s\n' "$1" >&2
    exit 1
  fi
}

need git
need make
need cc

work="$(mktemp -d "${TMPDIR:-/tmp}/tny-setup.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

git clone --depth 1 ${REF:+--branch "$REF"} "$REPO" "$work/tny"
make -C "$work/tny" release
mkdir -p "$PREFIX/bin"
cp "$work/tny/build/tny" "$PREFIX/bin/tny"
chmod 755 "$PREFIX/bin/tny"

printf 'installed %s (%s bytes)\n' "$PREFIX/bin/tny" "$(wc -c < "$PREFIX/bin/tny" | tr -d ' ')"
if ! command -v tny >/dev/null 2>&1; then
  printf 'add %s/bin to PATH to run tny\n' "$PREFIX" >&2
fi
tny --version 2>/dev/null || "$PREFIX/bin/tny" --version
