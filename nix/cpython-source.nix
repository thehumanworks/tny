# The pinned CPython source for Python code cells (docs/adr/0179). Version and
# hash come from third_party/cpython, the Makefile's single source of truth;
# the Nix sandbox then builds with CPYTHON_TARBALL and CPYTHON_FETCH=0.
{ lib, fetchurl }:

let
  version = lib.strings.trim (builtins.readFile ../third_party/cpython/VERSION);
in
fetchurl {
  url = "https://www.python.org/ftp/python/${version}/Python-${version}.tar.xz";
  sha256 = lib.strings.trim (builtins.readFile ../third_party/cpython/SHA256);
}
