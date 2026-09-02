# `nix develop` — everything the Makefile, the test suites, and the benches
# expect on PATH.
{
  lib,
  stdenv,
  mkShell,
  actionlint,
  clang-tools,
  git,
  gnumake,
  hyperfine,
  nodejs,
  openssl,
  patchelf,
  pkg-config,
  python3,
  ruff,
  shellcheck,
  shfmt,
  valgrind,
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
    # `make quality` (docs/adr/0039). nixpkgs pins these to whatever the
    # channel carries, not to the exact versions in .mise.toml and
    # .github/workflows/ci.yml, so a formatting diff here is a channel skew,
    # not a real failure — `mise install` is the version-exact path
    # (docs/adr/0061).
    actionlint
    clang-tools # clang-format, clang-tidy
    ruff
    shellcheck
    shfmt
  ]
  # `make leaks` / `make valgrind`. valgrind has no aarch64-darwin port, and
  # the leak gate falls back to /usr/bin/leaks there (docs/adr/0061).
  ++ lib.optionals stdenv.hostPlatform.isLinux [ valgrind ];

  # See nix/package.nix: the -O0 sanitizer build trips glibc's
  # "_FORTIFY_SOURCE requires compiling with optimization" warning, and the
  # Makefile is -Werror.
  hardeningDisable = [ "fortify" ];

  # Give everything the Makefile links the same OpenSSL RUNPATH the packaged
  # binary gets, so `make && ./build/tny` can dlopen libssl on a machine with
  # no system OpenSSL. Deliberately not LD_LIBRARY_PATH: that leaks into every
  # process started from the shell and shadows the host's own libraries.
  NIX_LDFLAGS = lib.optionalString stdenv.hostPlatform.isLinux "-rpath ${lib.getLib openssl}/lib";

  # See nix/tests.nix: those same link flags reach the `-fsyntax-only` header
  # check in tests/integration/test_libtny.py, where clang counts them as unused
  # arguments and -Werror turns them into a failure.
  NIX_CFLAGS_COMPILE = lib.optionalString stdenv.cc.isClang "-Wno-unused-command-line-argument";

  shellHook = ''
    echo "tny dev shell — make | make test | make quality | make leaks | make bench"
  '';
}
