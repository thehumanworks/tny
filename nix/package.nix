# The tny CLI/TUI binary plus the pure-Python extension host.
#
# Usable without flakes:  pkgs.callPackage ./nix/package.nix { }
{
  lib,
  stdenv,
  cacert,
  makeBinaryWrapper,
  openssl,
  python3,

  # ADR 0014 makes the git tag the only source of truth for the version and
  # `TNY_VERSION` the documented override for builds without git — which is
  # every Nix build, since the flake source has no .git. flake.nix passes the
  # revision; override it when packaging a tag:
  #   pkgs.tny.override { version = "0.2.1"; }
  version ? "0.0.0-unknown",

  # tny execs `python3` from PATH for the extension host and reads the system
  # CA bundle for TLS. Wrapping supplies both; set false for an unwrapped
  # binary that inherits the caller's environment untouched.
  wrapRuntime ? true,
}:

let
  src = (import ./source.nix { inherit lib; }).build;
in
stdenv.mkDerivation (finalAttrs: {
  pname = "tny";
  inherit version src;

  strictDeps = true;
  nativeBuildInputs = lib.optionals wrapRuntime [ makeBinaryWrapper ];

  # No buildInputs: tny links nothing beyond libc/libdl/libpthread. TLS is
  # dlopen'd at first use (docs/size-and-speed.md forbids linking OpenSSL), so
  # OpenSSL reaches the binary through RUNPATH in postFixup instead.

  enableParallelBuilding = true;
  dontConfigure = true;

  makeFlags = [
    "PREFIX=$(out)"
    "CC=${stdenv.cc.targetPrefix}cc"
    "TNY_VERSION=${finalAttrs.version}"
    "TNY_SHELL_PATH=${stdenv.shell}"
  ];

  postFixup =
    # src/net/stream.c dlopen()s "libssl.so.3" (then .so.1.1, then .so) at
    # first TLS use. Under Nix nothing is on a default library path, so glibc
    # resolves that dlopen through the calling object's RUNPATH. This has to
    # run in postFixup: the fixup hooks already ran `patchelf --shrink-rpath`,
    # which drops any entry no DT_NEEDED justifies.
    lib.optionalString stdenv.hostPlatform.isLinux ''
      patchelf --add-rpath ${lib.getLib openssl}/lib $out/bin/tny
    ''
    # macOS needs nothing here: the SecureTransport shim dlopen()s
    # /System/Library/Frameworks by absolute path.
    + lib.optionalString wrapRuntime ''
      wrapProgram $out/bin/tny \
        --suffix PATH : ${lib.makeBinPath [ python3 ]} \
        --set-default SSL_CERT_FILE ${cacert}/etc/ssl/certs/ca-bundle.crt
    '';

  # docs/size-and-speed.md is a product invariant, not a preference: the
  # stripped binary has a per-platform byte budget and the Makefile owns the
  # numbers. Runs against the pre-fixup binary, same as CI.
  doCheck = true;
  checkTarget = "size-check";

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck

    echo "$($out/bin/tny --version) is the built version"
    test "$($out/bin/tny --version)" = "${finalAttrs.version}"
    $out/bin/tny --help > /dev/null
    $out/bin/tny ask --help > /dev/null
    test -f $out/lib/tny/tny_extension_host.py
    test -f $out/share/tny/tny-workflows.sh
    payload=$out/bin/tny
    if test -x $out/bin/.tny-wrapped; then payload=$out/bin/.tny-wrapped; fi
    grep -aF '${stdenv.shell}' "$payload" > /dev/null
    if grep -aF '/bin/sh' "$payload" > /dev/null; then
      echo "error: Nix package retained a host /bin/sh dependency" >&2
      exit 1
    fi

    runHook postInstallCheck
  '';

  meta = {
    description = "Tiny C11 TUI and CLI coding-agent harness";
    longDescription = ''
      tny drives Cursor (SDK Bridge), Codex (app-server), any ACP agent, and
      OpenAI-compatible endpoints through one normalized event loop, from a
      stripped binary well under 1 MiB.

      Host agents stay external processes: install `cursor-sdk-bridge`,
      `codex`, or an ACP agent separately and put them on PATH.
    '';
    homepage = "https://github.com/thehumanworks/tny";
    # meta.license is deliberately unset. LICENSE-METADATA.json records
    # `LicenseRef-UNLICENSED` (all rights reserved), which has no nixpkgs
    # license attribute; using `lib.licenses.unfree` would make every consumer
    # pass allowUnfree for a first-party flake. Redistributors must read
    # LICENSE-METADATA.json and THIRD_PARTY_NOTICES.md — see docs/nix.md.
    mainProgram = "tny";
    platforms = lib.platforms.linux ++ lib.platforms.darwin;
  };
})
