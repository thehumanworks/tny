#!/bin/sh
# Install the license texts of everything the tny executable embeds
# (docs/adr/0179, docs/adr/0180) into DEST, one file per notice:
# THIRD_PARTY_NOTICES.md, CPython's license and the licenses of the software
# CPython incorporates, and each pinned library in third_party/<name>/ that
# has a URL (built by scripts/cpython_runtime.sh) as <name>-<file>.
#
#   scripts/install_licenses.sh DEST
set -eu

dest=$1
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$dest"
cp "$root/THIRD_PARTY_NOTICES.md" "$dest/"
cp "$root/third_party/cpython/LICENSE" "$dest/CPython-LICENSE"
cp "$root/third_party/cpython/LICENSE-incorporated.rst" "$dest/CPython-incorporated-software.rst"
cp "$root/third_party/cpython/HACL-LICENSE" "$dest/HACL-LICENSE"
for pin in "$root"/third_party/*/URL; do
    dir=$(dirname "$pin")
    name=$(basename "$dir")
    for file in "$dir"/LICENSE* "$dir"/COPYING*; do
        if [ -f "$file" ]; then cp "$file" "$dest/$name-$(basename "$file")"; fi
    done
done
