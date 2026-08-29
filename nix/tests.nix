# `make test` as a derivation: the greatest unit suite under ASan/UBSan, the
# event-schema and conformance-contract checks, and the fixture-driven
# integration suite for every backend. No live keys, no network (AGENTS.md).
{
  lib,
  stdenv,
  darwin,
  bash,
  nodejs,
  openssl,
  procps,
  python3,
  zsh,
  version ? "0.0.0-unknown",
}:

let
  src = (import ./source.nix { inherit lib; }).tests;
in
stdenv.mkDerivation {
  name = "tny-tests-${version}";
  inherit src;

  strictDeps = true;
  nativeBuildInputs = [
    bash
    python3
    zsh
    nodejs # tests/site/test_term.js, driven by test_site.py
    openssl.bin # tests/integration/test_https.py mints a throwaway cert
  ]
  # tests/integration/test_tui.py reads `ps` to prove the TUI pre-warm spawned
  # exactly one host. A builder's PATH holds only its inputs, so macOS needs an
  # explicit ps too — /bin/ps is on the disk but never on the PATH.
  ++ lib.optionals stdenv.hostPlatform.isLinux [ procps ]
  ++ lib.optionals stdenv.hostPlatform.isDarwin [ darwin.ps ];

  # The debug/test binary is -O0, and glibc's features.h emits a #warning when
  # _FORTIFY_SOURCE is set without optimization. The Makefile builds -Werror.
  hardeningDisable = [ "fortify" ];

  # cc-wrapper appends its link flags to every call it cannot prove is
  # compile-only, and `-fsyntax-only` is not on that list. The installed-header
  # check in tests/integration/test_libtny.py compiles stdin with -Werror and so
  # sees a stack of -L store paths it never links: clang reports each as
  # "argument unused during compilation" and the check fails. gcc says nothing,
  # hence the guard rather than an unconditional flag.
  NIX_CFLAGS_COMPILE = lib.optionalString stdenv.cc.isClang "-Wno-unused-command-line-argument";

  # The TLS suites assert a real handshake failure, not "libssl not found", so
  # the dlopen in src/net/stream.c has to resolve here too. RUNPATH — what the
  # installed package uses — is not enough: ASan intercepts dlopen, so glibc
  # resolves it against libasan's RUNPATH instead of the test binary's. The
  # search path is the only lever that reaches both binaries, and it is inert
  # for the mock servers, whose Python links this very OpenSSL.
  LD_LIBRARY_PATH = lib.optionalString stdenv.hostPlatform.isLinux "${lib.getLib openssl}/lib";

  enableParallelBuilding = true;
  dontConfigure = true;

  # The fixture agents and mock hosts are `#!/usr/bin/env python3` scripts that
  # tny execs directly; /usr/bin/env does not exist in the sandbox.
  postPatch = ''
    patchShebangs tests examples scripts shell
  '';

  makeFlags = [
    "CC=${stdenv.cc.targetPrefix}cc"
    "TNY_VERSION=${version}"
    "TNY_SHELL_PATH=${stdenv.shell}"
    "BASH=${bash}/bin/bash"
    "ZSH=${zsh}/bin/zsh"
  ] ++ lib.optionals stdenv.hostPlatform.isDarwin [
    # Keep the displayed flake revision while supplying dyld's numeric field
    # to the active-library integration tests.
    "LIBTNY_MACH_CURRENT_VERSION=1.0.0"
  ];
  buildFlags = [ "test" "test-shell-workflows" ];

  # `make test` is the whole point; there is nothing to install.
  installPhase = ''
    runHook preInstall
    grep -aF '${stdenv.shell}' build/tny > /dev/null
    if grep -aF '/bin/sh' build/tny > /dev/null; then
      echo "error: Nix tests retained a host /bin/sh dependency" >&2
      exit 1
    fi
    mkdir -p "$out"
    cp -r build/generated "$out/generated"
    runHook postInstall
  '';
}
