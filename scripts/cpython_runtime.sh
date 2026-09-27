#!/bin/sh
# Build the pinned, self-contained CPython runtime used by Python code mode
# (docs/adr/0179). Produces a static libpython plus generated frozen-module
# headers; nothing is installed and no system Python is used at run time.
#
#   scripts/cpython_runtime.sh OUT_DIR TARBALL SHA256 URL
#
# CC, CPYTHON_CFLAGS and CPYTHON_JOBS come from the environment (the Makefile
# passes its own compiler). A missing TARBALL is downloaded from URL only when
# TNY_CPYTHON_FETCH=1; otherwise the build fails with instructions. A tarball
# whose SHA256 differs is always rejected, never substituted.
set -eu

out=$1
tarball=$2
expected=$3
url=$4
cc=${CC:-cc}
cflags=${CPYTHON_CFLAGS:--Os -ffunction-sections -fdata-sections}
jobs=${CPYTHON_JOBS:-4}

digest() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

if [ ! -f "$tarball" ]; then
    if [ "${TNY_CPYTHON_FETCH:-0}" != 1 ]; then
        echo "error: missing pinned CPython source $tarball" >&2
        echo "Fetch it with: make fetch-cpython (or pass CPYTHON_TARBALL=/path)." >&2
        exit 1
    fi
    mkdir -p "$(dirname "$tarball")"
    curl -fsSL --retry 3 -o "$tarball.part" "$url"
    mv "$tarball.part" "$tarball"
fi
actual=$(digest "$tarball")
if [ "$actual" != "$expected" ]; then
    echo "error: $tarball SHA256 $actual does not match pinned $expected" >&2
    exit 1
fi

rm -rf "$out.tmp"
mkdir -p "$out.tmp"
tar -xJf "$tarball" -C "$out.tmp"
src=$(find "$out.tmp" -mindepth 1 -maxdepth 1 -type d -name 'Python-*' | head -n 1)
test -n "$src"

# Only the interpreter core and its bootstrap modules are linked. Optional
# stdlib extension modules, ensurepip, test modules, docstrings, mimalloc and
# the remote-debugging attach surface are not built.
(
    cd "$src"
    # A parent make's jobserver/flags must not leak into CPython's own build.
    unset MAKEFLAGS MFLAGS MAKELEVEL
    # libintl (e.g. Homebrew gettext on macOS) would become a dynamic
    # dependency of the executable; the code cell never uses gettext.
    ac_cv_lib_intl_textdomain=no ac_cv_header_libintl_h=no \
        CC=$cc CFLAGS=$cflags ./configure \
        --disable-shared \
        --without-ensurepip \
        --disable-test-modules \
        --without-doc-strings \
        --without-mimalloc \
        --without-remote-debug \
        --without-readline \
        --disable-ipv6 > configure.log 2>&1 ||
        {
            tail -n 40 configure.log >&2
            exit 1
        }
    make -j"$jobs" libpython3.14.a Programs/_freeze_module > make.log 2>&1 ||
        {
            tail -n 60 make.log >&2
            exit 1
        }
    # System libraries the static core and its bootstrap modules link against.
    # shellcheck disable=SC2016 # make, not the shell, expands these variables
    printf 'tny-ldlibs:\n\t@echo $(LOCALMODLIBS) $(LIBS) $(SYSLIBS)\n' > tny-ldlibs.mk
    # The core never calls CoreFoundation (only the unbuilt _scproxy does), and
    # tny links no framework eagerly.
    make -s -f Makefile -f tny-ldlibs.mk tny-ldlibs |
        sed 's/-framework CoreFoundation//g' > ldlibs
)

mkdir -p "$out.tmp/include"
for module in encodings:__init__ encodings.aliases:aliases encodings.utf_8:utf_8; do
    name=${module%%:*}
    file=${module#*:}
    header=$(printf '%s' "$name" | tr . _)
    "$src/Programs/_freeze_module" "$name" "$src/Lib/encodings/$file.py" \
        "$out.tmp/include/tny_frozen_$header.h"
done

rm -rf "$out"
mkdir -p "$out"
mv "$src/libpython3.14.a" "$src/ldlibs" "$out/"
mv "$out.tmp/include" "$out/frozen"
mkdir -p "$out/include"
cp -R "$src/Include/." "$out/include/"
cp "$src/pyconfig.h" "$out/include/"
cp "$src/LICENSE" "$out/LICENSE"
rm -rf "$out.tmp"
printf '%s\n' "$expected" > "$out/stamp"
