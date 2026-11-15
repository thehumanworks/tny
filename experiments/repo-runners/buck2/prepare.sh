#!/bin/sh
set -eu
cd "$(dirname "$0")"
archive=$(mktemp ./source.tar.XXXXXX)
trap 'rm -f "$archive"' EXIT HUP INT TERM
git -C ../../.. archive HEAD > "$archive"
if ! cmp -s "$archive" source.tar; then mv "$archive" source.tar; fi
