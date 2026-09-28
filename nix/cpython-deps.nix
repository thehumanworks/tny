# Pinned native libraries behind the embedded CPython's stdlib extension
# modules (docs/adr/0180): ssl/hashlib, zlib, bz2, lzma, compression.zstd,
# sqlite3 and ctypes. URL and SHA256 come from third_party/<name>/, the
# Makefile's single source of truth. The result is the CPYTHON_DEPS_DIR that
# scripts/cpython_runtime.sh reads tarballs from by file name, so the sandbox
# builds with CPYTHON_FETCH=0 and no network.
{
  lib,
  fetchurl,
  linkFarm,
}:

let
  names = [
    "zlib"
    "bzip2"
    "xz"
    "zstd"
    "sqlite"
    "libffi"
    "openssl"
  ];
  pin = name: file: lib.strings.trim (builtins.readFile (../third_party + "/${name}/${file}"));
  entry =
    name:
    let
      url = pin name "URL";
    in
    {
      name = baseNameOf url;
      path = fetchurl {
        inherit url;
        sha256 = pin name "SHA256";
      };
    };
in
linkFarm "tny-cpython-deps" (map entry names)
