#!/bin/sh
set -eu
source_archive="$PWD/$1"
output="$PWD/$2"
cc=$3
cxx=$4
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
tar -xf "$source_archive" -C "$work"
cd "$work"
# Compile from clean source. Buck owns the output; Make owns its internal graph.
make -j4 CC="$cc" CXX="$cxx" TNY_VERSION=0.39.0 CPYTHON_JOBS=4
mkdir -p "$(dirname "$output")"
cp build/tny "$output"
