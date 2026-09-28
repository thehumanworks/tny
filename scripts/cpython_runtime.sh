#!/bin/sh
# Build the pinned, self-contained CPython runtime used by Python code mode
# (docs/adr/0179, docs/adr/0180). Produces static archives plus a frozen
# standard library; nothing is installed and no system Python, stdlib
# directory or host development library is used, at build or run time.
#
#   scripts/cpython_runtime.sh OUT_DIR TARBALL SHA256 URL
#
# CC, CPYTHON_CFLAGS and CPYTHON_JOBS come from the environment (the Makefile
# passes its own compiler). The native libraries behind stdlib extension
# modules are pinned in third_party/<name>/{URL,SHA256}; their tarballs live
# in TNY_CPYTHON_DEPS_DIR under the URL's file name. A missing tarball is
# downloaded only when TNY_CPYTHON_FETCH=1; otherwise the build fails with
# instructions. A tarball whose SHA256 differs is always rejected.
#
# Output layout:
#   libpython3.14.a   interpreter plus every buildable stdlib extension module,
#                     linked in statically (MODULE_BUILDTYPE=static)
#   lib/*.a           the frozen pure-Python stdlib, the pinned libraries and
#                     the archives CPython vendors (expat, libmpdec, HACL*)
#   ldlibs            link arguments that follow libpython3.14.a
#   include/          CPython headers
#   frozen/           modules.txt: every frozen stdlib module name
#   licenses/         license texts of everything linked in
set -eu

out=$1
tarball=$2
expected=$3
url=$4
cc=${CC:-cc}
cflags=${CPYTHON_CFLAGS:--Os -ffunction-sections -fdata-sections}
jobs=${CPYTHON_JOBS:-4}
deps=${TNY_CPYTHON_DEPS_DIR:-build/deps}
root=$(cd "$(dirname "$0")/.." && pwd)
# Linked into the interpreter, in dependency order for static linking.
libraries="zlib bzip2 xz zstd sqlite libffi openssl"

digest() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# verified TARBALL SHA256 URL: fetch when allowed, then require the pin.
verified() {
    if [ ! -f "$1" ]; then
        if [ "${TNY_CPYTHON_FETCH:-0}" != 1 ]; then
            echo "error: missing pinned source $1" >&2
            echo "Fetch it with: make fetch-cpython (or pass CPYTHON_DEPS_DIR=/dir)." >&2
            exit 1
        fi
        mkdir -p "$(dirname "$1")"
        curl -fsSL --retry 3 -o "$1.part" "$3"
        mv "$1.part" "$1"
    fi
    actual=$(digest "$1")
    if [ "$actual" != "$2" ]; then
        echo "error: $1 SHA256 $actual does not match pinned $2" >&2
        exit 1
    fi
}

# unpack TARBALL DIR: extract and print the single top-level source directory.
unpack() {
    mkdir -p "$2"
    case $1 in
        *.tar.xz) tar -xJf "$1" -C "$2" ;;
        *) tar -xzf "$1" -C "$2" ;;
    esac
    find "$2" -mindepth 1 -maxdepth 1 -type d | head -n 1
}

# quiet LOG COMMAND...: run with output in LOG, showing its tail on failure.
quiet() {
    log=$1
    shift
    if ! "$@" > "$log" 2>&1; then
        tail -n 60 "$log" >&2
        exit 1
    fi
}

pin() { tr -d ' \n' < "$root/third_party/$1/$2"; }

verified "$tarball" "$expected" "$url"
for name in $libraries; do
    link=$(pin "$name" URL)
    verified "$deps/${link##*/}" "$(pin "$name" SHA256)" "$link"
done

rm -rf "$out.tmp"
mkdir -p "$out.tmp/stage/include" "$out.tmp/stage/lib"
work=$(cd "$out.tmp" && pwd)
stage=$work/stage
# A parent make's jobserver/flags must not leak into the dependency builds.
unset MAKEFLAGS MFLAGS MAKELEVEL
# Autotools packages: static library only, no programs, docs or translations.
autotools() {
    quiet configure.log env CC="$cc" CFLAGS="$cflags" ./configure --prefix="$stage" \
        --libdir="$stage/lib" --disable-shared --enable-static --disable-dependency-tracking "$@"
    quiet make.log make -j"$jobs"
    quiet install.log make install
}

for name in $libraries; do
    link=$(pin "$name" URL)
    dir=$(unpack "$deps/${link##*/}" "$work/$name")
    (
        cd "$dir"
        case $name in
            zlib)
                quiet configure.log env CC="$cc" CFLAGS="$cflags" ./configure --static \
                    --prefix="$stage"
                quiet make.log make -j"$jobs" libz.a
                quiet install.log make install
                ;;
            bzip2)
                quiet make.log make -j"$jobs" libbz2.a CC="$cc" \
                    CFLAGS="$cflags -D_FILE_OFFSET_BITS=64"
                cp libbz2.a "$stage/lib/"
                cp bzlib.h "$stage/include/"
                ;;
            xz)
                autotools --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo \
                    --disable-lzma-links --disable-scripts --disable-doc --disable-nls
                ;;
            zstd)
                # Multithreaded (compression.zstd workers); MOREFLAGS keeps
                # zstd's own flags, including its non-executable-stack note.
                quiet make.log make -C lib -j"$jobs" libzstd.a-mt CC="$cc" MOREFLAGS="$cflags"
                cp lib/libzstd.a "$stage/lib/"
                cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$stage/include/"
                ;;
            sqlite)
                # The amalgamation needs no build system. Extension loading
                # (dlopen of arbitrary libraries) is omitted, as in CPython's
                # default configuration.
                # shellcheck disable=SC2086 # cflags is a flag list
                quiet make.log "$cc" $cflags -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
                    -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_MATH_FUNCTIONS \
                    -DSQLITE_ENABLE_DBSTAT_VTAB -c sqlite3.c -o sqlite3.o
                ar rc "$stage/lib/libsqlite3.a" sqlite3.o
                ranlib "$stage/lib/libsqlite3.a"
                cp sqlite3.h sqlite3ext.h "$stage/include/"
                ;;
            libffi)
                autotools --disable-docs --disable-multi-os-directory
                ;;
            openssl)
                # OPENSSLDIR=/etc/ssl is the common system CA location; the
                # cell sets SSL_CERT_FILE only when that default is absent
                # (src/core/code_python.c). No engines, provider modules,
                # apps or tests.
                quiet configure.log ./Configure no-shared no-module no-dso no-engine no-tests \
                    no-apps no-docs --prefix="$stage" --libdir=lib --openssldir=/etc/ssl \
                    CC="$cc" CFLAGS="$cflags"
                quiet make.log make -j"$jobs" build_libs
                quiet install.log make install_dev
                ;;
        esac
    )
done

src=$(unpack "$tarball" "$work/cpython")
(
    cd "$src"
    # MODULE_BUILDTYPE=static links every buildable stdlib extension module
    # into libpython, against the pinned libraries above (explicit *_CFLAGS/
    # *_LIBS, never pkg-config). Modules without a pinned library are
    # disabled explicitly, so a host development package can never become a
    # dependency of tny: curses/readline (terminal UI), gdbm/ndbm, Tk and
    # libuuid (uuid works without it). libintl (e.g. Homebrew gettext) would
    # also be dynamic; the cell never uses gettext's C library. _scproxy needs
    # macOS frameworks, which tny does not link; a frozen shim replaces it
    # (see below). macOS would otherwise prefer the SDK's libffi.
    quiet configure.log env ac_cv_lib_intl_textdomain=no ac_cv_header_libintl_h=no \
        ac_cv_lib_ffi_ffi_call=no \
        py_cv_module__uuid=n/a py_cv_module__dbm=n/a py_cv_module__gdbm=n/a \
        py_cv_module_readline=n/a py_cv_module__curses=n/a py_cv_module__curses_panel=n/a \
        py_cv_module__tkinter=n/a py_cv_module__scproxy=n/a \
        MODULE_BUILDTYPE=static \
        ZLIB_CFLAGS="-I$stage/include" ZLIB_LIBS="-L$stage/lib -lz" \
        BZIP2_CFLAGS="-I$stage/include" BZIP2_LIBS="-L$stage/lib -lbz2" \
        LIBLZMA_CFLAGS="-I$stage/include" LIBLZMA_LIBS="-L$stage/lib -llzma" \
        LIBZSTD_CFLAGS="-I$stage/include" LIBZSTD_LIBS="-L$stage/lib -lzstd -lpthread" \
        LIBSQLITE3_CFLAGS="-I$stage/include" LIBSQLITE3_LIBS="-L$stage/lib -lsqlite3 -lm -lpthread" \
        LIBFFI_CFLAGS="-I$stage/include" LIBFFI_LIBS="-L$stage/lib -lffi" \
        PKG_CONFIG=false \
        CC="$cc" CFLAGS="$cflags" ./configure \
        --disable-shared \
        --without-ensurepip \
        --disable-test-modules \
        --without-mimalloc \
        --without-remote-debug \
        --without-readline \
        --with-system-expat=no \
        --with-system-libmpdec=no \
        --with-openssl="$stage" \
        --with-openssl-rpath=no
    # The build-tree interpreter freezes the stdlib with this exact runtime's
    # marshal format and generates the _sysconfigdata module.
    quiet make.log make -j"$jobs" libpython3.14.a python pybuilddir.txt
    # Link arguments recorded for the static modules and the core.
    # shellcheck disable=SC2016 # make, not the shell, expands these variables
    printf 'tny-ldlibs:\n\t@echo $(LOCALMODLIBS) $(LIBS) $(SYSLIBS)\n' > tny-ldlibs.mk
    make -s -f Makefile -f tny-ldlibs.mk tny-ldlibs > ldlibs.raw
)

# Every module with a pinned library must have been built: a silently skipped
# module would ship a smaller stdlib than documented.
for module in zlib _bz2 _lzma _zstd _sqlite3 _ctypes _ssl _hashlib; do
    grep -q "^$module " "$src/Modules/Setup.stdlib" || {
        echo "error: CPython did not enable $module" >&2
        exit 1
    }
done

mkdir -p "$work/out/lib" "$work/out/licenses"
builddir=$(cat "$src/pybuilddir.txt")
extras=$(find "$src/$builddir" -name '_sysconfigdata_*.py')
if [ "$(uname -s)" = Darwin ]; then
    # urllib.request imports _scproxy unconditionally on macOS. Without the
    # CoreFoundation/SystemConfiguration link it reports no system proxy
    # configuration; proxy environment variables still apply.
    printf '%s\n' '"""tny: macOS system proxy settings are not read; use *_proxy env vars."""' \
        'def _get_proxy_settings():' \
        '    return {"exclude_simple": False, "exceptions": ()}' \
        'def _get_proxies():' '    return {}' > "$work/_scproxy.py"
    extras="$extras $work/_scproxy.py"
fi
# shellcheck disable=SC2086 # extras is a list of generated module paths
"$src/python" -E -S "$root/scripts/cpython_freeze_stdlib.py" "$src/Lib" "$work/frozen" $extras
(
    cd "$work/frozen"
    # shellcheck disable=SC2086 # cflags is a flag list
    find . -name '*.c' -print0 | xargs -0 -n 32 -P "$jobs" "$cc" $cflags -c
    find . -name '*.o' -exec ar rc "$work/out/lib/libtny_pystdlib.a" {} +
)
ranlib "$work/out/lib/libtny_pystdlib.a"

# Rewrite recorded arguments: vendored archives and pinned -l libraries become
# explicit archive paths under OUT_DIR, so no -L search can pick a host copy.
ldlibs="$out/lib/libtny_pystdlib.a"
framework=
# shellcheck disable=SC2013 # link arguments are whitespace-separated words
for arg in $(cat "$src/ldlibs.raw"); do
    if [ -n "$framework" ]; then
        # The core never calls CoreFoundation (only the unbuilt _scproxy does).
        [ "$arg" = CoreFoundation ] || ldlibs="$ldlibs -framework $arg"
        framework=
        continue
    fi
    case $arg in
        -L*) continue ;;
        -framework)
            framework=1
            continue
            ;;
        -lz | -lbz2 | -llzma | -lzstd | -lsqlite3 | -lffi | -lssl | -lcrypto)
            name=lib${arg#-l}.a
            cp "$stage/lib/$name" "$work/out/lib/$name"
            next="$out/lib/$name"
            ;;
        *.a)
            name=$(basename "$arg")
            cp "$src/$arg" "$work/out/lib/$name"
            next="$out/lib/$name"
            ;;
        *) next=$arg ;;
    esac
    case " $ldlibs " in
        *" $next "*) case $next in -l*) ldlibs="$ldlibs $next" ;; esac ;;
        *) ldlibs="$ldlibs $next" ;;
    esac
done
# The static libraries' own system needs (threads for zstd/sqlite, libm).
ldlibs="$ldlibs -lpthread -lm"

# Licenses of everything linked into tny: CPython and the software it
# incorporates (expat, libmpdec, HACL* and others per Doc/license.rst) and the
# pinned libraries.
licenses=$work/out/licenses
cp "$src/LICENSE" "$licenses/CPython-LICENSE"
cp "$src/Doc/license.rst" "$licenses/CPython-incorporated-software.rst"
cp "$src/Modules/expat/COPYING" "$licenses/expat-COPYING"
sed -n '1,/\*\//p' "$src/Modules/_hacl/Hacl_Hash_SHA2.c" > "$licenses/HACL-LICENSE"
for name in $libraries; do
    for file in "$root/third_party/$name"/LICENSE* "$root/third_party/$name"/COPYING*; do
        if [ -f "$file" ]; then cp "$file" "$licenses/$name-${file##*/}"; fi
    done
done

rm -rf "$out"
mkdir -p "$out"
mv "$src/libpython3.14.a" "$out/"
mv "$work/out/lib" "$licenses" "$out/"
printf '%s\n' "$ldlibs" > "$out/ldlibs"
mkdir -p "$out/frozen" "$out/include"
cp "$work/frozen/modules.txt" "$out/frozen/"
cp -R "$src/Include/." "$out/include/"
cp "$src/pyconfig.h" "$out/include/"
cp "$src/LICENSE" "$out/LICENSE"
rm -rf "$out.tmp"
{
    printf 'cpython %s\n' "$expected"
    for name in $libraries; do printf '%s %s\n' "$name" "$(pin "$name" SHA256)"; done
} > "$out/stamp"
