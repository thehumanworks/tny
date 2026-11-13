#!/bin/sh
# Install the license texts that ship with tny into DEST: tny's own LICENSE
# (MIT) and THIRD_PARTY_NOTICES.md for the vendored sources it embeds. No
# language runtime is embedded (docs/adr/0222).
#
#   scripts/install_licenses.sh DEST
set -eu

dest=$1
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$dest"
cp "$root/LICENSE" "$dest/LICENSE"
cp "$root/THIRD_PARTY_NOTICES.md" "$dest/"
