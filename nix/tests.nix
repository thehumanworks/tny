# `make test` as a derivation: the greatest unit suite under ASan/UBSan, the
# event-schema and conformance-contract checks, and the fixture-driven
# integration suite for every backend. No live keys, no network (AGENTS.md).
{
  lib,
  stdenv,
  nodejs,
  openssl,
  procps,
  python3,
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
    python3
    nodejs # tests/site/test_term.js, driven by test_site.py
    openssl.bin # tests/integration/test_https.py mints a throwaway cert
  ]
  # tests/integration/test_tui.py reads `ps` to prove the TUI pre-warm spawned
  # exactly one host. Darwin's ps is in the sandbox already.
  ++ lib.optionals stdenv.hostPlatform.isLinux [ procps ];

  # The debug/test binary is -O0, and glibc's features.h emits a #warning when
  # _FORTIFY_SOURCE is set without optimization. The Makefile builds -Werror.
  hardeningDisable = [ "fortify" ];

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
    patchShebangs tests examples scripts
  '';

  makeFlags = [
    "CC=${stdenv.cc.targetPrefix}cc"
    "TNY_VERSION=${version}"
  ];
  buildFlags = [ "test" ];

  # `make test` is the whole point; there is nothing to install.
  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    cp -r build/generated "$out/generated"
    runHook postInstall
  '';
}
