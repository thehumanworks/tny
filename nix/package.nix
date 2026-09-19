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
  tini,

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
  sources = import ./source.nix { inherit lib; };
  src = sources.build;
in
stdenv.mkDerivation (finalAttrs: {
  pname = "tny";
  inherit version src;

  strictDeps = true;
  nativeBuildInputs = lib.optionals wrapRuntime [ makeBinaryWrapper ];
  nativeInstallCheckInputs = lib.optionals stdenv.hostPlatform.isLinux [
    python3
    openssl.bin
    tini
  ];

  # stdenv supplies matching C/C++ drivers and the C++ runtime. TLS is
  # dlopen'd at first use (docs/size-and-speed.md forbids linking OpenSSL).
  # Preserve the linker-created layout: adding RUNPATH after stripping can
  # create another 64 KiB-aligned LOAD segment (ADR 0103).
  NIX_LDFLAGS = lib.optionalString stdenv.hostPlatform.isLinux "-rpath ${lib.getLib openssl}/lib";
  # The pinned patchelf hook only shrinks RUNPATH. It would remove the
  # deliberate dlopen-only path, so retain that path and the linker's other
  # store search paths. Recheck this hook when updating nixpkgs.
  dontPatchELF = stdenv.hostPlatform.isLinux;

  enableParallelBuilding = true;
  dontConfigure = true;

  # nixpkgs' default hardening set adds -fzero-call-used-regs=used-gpr, which
  # the Ubuntu-built release binaries never carry; on aarch64 its ~15 KiB of
  # register clearing pushed the read-only segment past a 64 KiB boundary and
  # the RELRO alignment then added a whole page to the file (ADR 0111). The
  # rest of the set (PIE, RELRO, bindnow, stack protector, stack clash,
  # fortify, format) stays.
  hardeningDisable = [ "zerocallusedregs" ];

  makeFlags = [
    "PREFIX=$(out)"
    "CC=${stdenv.cc.targetPrefix}cc"
    "CXX=${stdenv.cc.targetPrefix}c++"
    "TNY_VERSION=${finalAttrs.version}"
    "TNY_SHELL_PATH=${stdenv.shell}"
  ];

  postFixup = lib.optionalString wrapRuntime ''
    wrapProgram $out/bin/tny \
      --suffix PATH : ${lib.makeBinPath [ python3 ]} \
      --set-default SSL_CERT_FILE ${cacert}/etc/ssl/certs/ca-bundle.crt
  '';

  # Validate and measure the pre-fixup artifact, same as CI; no byte ceiling.
  doCheck = true;
  checkTarget = "size-check";

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck

    echo "$($out/bin/tny --version) is the built version"
    test "$($out/bin/tny --version)" = "${finalAttrs.version}"
    $out/bin/tny --help > /dev/null
    $out/bin/tny ask --help > /dev/null
    $out/bin/tny doctor --json > /dev/null
    test -f $out/lib/tny/tny_extension_host.py
    test -f $out/lib/tny/tny_improve.py
    test -f $out/lib/tny/tny_improve_propose.py
    test -f $out/share/tny/tny-workflows.sh
    payload=$out/bin/tny
    if test -x $out/bin/.tny-wrapped; then payload=$out/bin/.tny-wrapped; fi
    test -f "$payload" && test -s "$payload" && test -x "$payload"
    payload_bytes=$(wc -c < "$payload")
    echo "$payload_bytes $payload (installed payload)"
    make --no-print-directory $makeFlags -o release size-check BIN="$payload"
    grep -aF '${stdenv.shell}' "$payload" > /dev/null
    if grep -aF '/bin/sh' "$payload" > /dev/null; then
      echo "error: Nix package retained a host /bin/sh dependency" >&2
      exit 1
    fi

    ${lib.optionalString stdenv.hostPlatform.isLinux ''
      env -u LD_LIBRARY_PATH ${tini}/bin/tini -s -- \
        ${python3}/bin/python3 ${sources.packageChecks}/tests/integration/test_https.py \
        "$out/bin/tny"
    ''}

    runHook postInstallCheck
  '';

  meta = {
    description = "Tiny C11 TUI and CLI coding-agent harness";
    longDescription = ''
      tny runs its native agent loop over OpenAI-compatible HTTP, including
      Codex ChatGPT subscriptions and Grok public/subscription profiles.
      Named gateways use environment API keys. No vendor agent executable
      is required for inference or native subscription login/refresh.
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
