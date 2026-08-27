# `nix develop` — everything the Makefile, the test suites, and the benches
# expect on PATH.
{
  lib,
  stdenv,
  mkShell,
  git,
  gnumake,
  hyperfine,
  nodejs,
  openssl,
  patchelf,
  pkg-config,
  python3,
}:

mkShell {
  packages = [
    git
    gnumake
    hyperfine # make bench
    nodejs # sdk/typescript, tests/site
    openssl.bin # tests/integration/test_https.py mints a throwaway cert
    patchelf
    pkg-config
    python3 # integration fixtures, extension host, make site
  ];

  # See nix/package.nix: the -O0 sanitizer build trips glibc's
  # "_FORTIFY_SOURCE requires compiling with optimization" warning, and the
  # Makefile is -Werror.
  hardeningDisable = [ "fortify" ];

  # Give everything the Makefile links the same OpenSSL RUNPATH the packaged
  # binary gets, so `make && ./build/tny` can dlopen libssl on a machine with
  # no system OpenSSL. Deliberately not LD_LIBRARY_PATH: that leaks into every
  # process started from the shell and shadows the host's own libraries.
  NIX_LDFLAGS = lib.optionalString stdenv.hostPlatform.isLinux "-rpath ${lib.getLib openssl}/lib";

  shellHook = ''
    echo "tny dev shell — make | make test | make size-check | make bench"
  '';
}
