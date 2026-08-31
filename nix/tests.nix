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
  perl,
  procps,
  python3,
  util-linux,
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
    # Integration fixtures plus the optional stdlib-only
    # tnytty/tests/bench/bench_tnytty.py runner. The performance benchmark is
    # intentionally not part of buildPhase because shared CI timing is noisy.
    python3
    zsh
    nodejs # tests/site/test_term.js, driven by test_site.py
    openssl.bin # tests/integration/test_https.py mints a throwaway cert
    # shell/tny-workflows.sh launches each task in its own process group so the
    # scheduler can signal the whole tree. It uses setsid, falls back to perl's
    # POSIX::setsid, and fails the task outright when neither is on the PATH.
    # A builder's PATH holds only its inputs, so tests/shell/test_workflows.sh
    # needs both named here: util-linux for the setsid path (Linux only; it is
    # what a Linux user has), perl for the fallback path everywhere else.
    perl
  ]
  # tests/integration/test_tui.py reads `ps` to prove the TUI pre-warm spawned
  # exactly one host. A builder's PATH holds only its inputs, so macOS needs an
  # explicit ps too — /bin/ps is on the disk but never on the PATH.
  ++ lib.optionals stdenv.hostPlatform.isLinux [ procps util-linux ]
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

  # `test` compiles the ASan unit suite; `test-shell-workflows` is a
  # process-group scheduler. Putting both in buildFlags lets -jN interleave
  # them, and a loaded builder can expire the 1s TERM grace before a
  # cooperative child's EXIT trap records the active-count drop.
  buildPhase = ''
    runHook preBuild
    for fixture in codex.toml claude-user.json claude-project.json \
      grok.toml grok-project.toml cursor-user.json cursor-project.json \
      malformed.json malformed.toml; do
      test -f "tests/fixtures/mcp-import/$fixture" || {
        echo "error: missing MCP import fixture $fixture" >&2
        exit 1
      }
    done
    make -j''${NIX_BUILD_CORES} $makeFlags test
    make $makeFlags test-shell-workflows
    runHook postBuild
  '';

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
