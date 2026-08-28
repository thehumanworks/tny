# libtny: the active ABI-1 headless embedding library (docs/libtny.md,
# ADR 0037) — shared library, public header, and pkg-config metadata. The
# frozen ABI-0 compatibility package remains a release artifact; a Nix source
# build intentionally needs no historical Git object or network fetch.
#
# Usable without flakes:  pkgs.callPackage ./nix/libtny.nix { }
{
  lib,
  stdenv,
  openssl,
  pkg-config,
  python3,

  # See the note in package.nix: Nix builds have no git, so ADR 0014's
  # TNY_VERSION override is the supported path.
  version ? "0.0.0-unknown",
}:

let
  src = (import ./source.nix { inherit lib; }).build;
in
stdenv.mkDerivation (finalAttrs: {
  pname = "libtny";
  inherit version src;

  strictDeps = true;
  nativeBuildInputs = [
    pkg-config # installCheck only
    python3 # ABI baseline and Mach-O version validation
  ];

  enableParallelBuilding = true;
  dontConfigure = true;

  makeFlags = [
    "PREFIX=$(out)"
    "CC=${stdenv.cc.targetPrefix}cc"
    "TNY_VERSION=${finalAttrs.version}"
    "TNY_SHELL_PATH=${stdenv.shell}"
  ] ++ lib.optionals stdenv.hostPlatform.isDarwin [
    # A flake revision is intentionally the displayed product version, but a
    # bare hash is not representable as Mach-O's numeric current_version.
    "LIBTNY_MACH_CURRENT_VERSION=1.0.0"
  ];
  buildFlags = [ "lib-shared-active" ];
  installTargets = [ "install-lib-active" ];

  # As in package.nix: TLS is dlopen'd, so the store path has to be in RUNPATH
  # and has to be added after `patchelf --shrink-rpath` has run.
  postFixup = lib.optionalString stdenv.hostPlatform.isLinux ''
    patchelf --add-rpath ${lib.getLib openssl}/lib $out/lib/libtny.so.1
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck

    library=$out/lib/${if stdenv.hostPlatform.isDarwin then "libtny.1.dylib" else "libtny.so.1"}
    test -f $out/include/tny/tny.h
    test -f "$library"
    grep -aF '${stdenv.shell}' "$library" > /dev/null
    if grep -aF '/bin/sh' "$library" > /dev/null; then
      echo "error: Nix libtny retained a host /bin/sh dependency" >&2
      exit 1
    fi
    PKG_CONFIG_PATH=$out/lib/pkgconfig \
      pkg-config --exists --print-errors libtny
    test "$(PKG_CONFIG_PATH=$out/lib/pkgconfig pkg-config --modversion libtny)" \
      = "${finalAttrs.version}"

    runHook postInstallCheck
  '';

  meta = {
    description = "Experimental headless embedding ABI for the tny agent harness";
    homepage = "https://github.com/thehumanworks/tny";
    # See the license note in package.nix.
    pkgConfigModules = [ "libtny" ];
    # ABI 1 supports macOS arm64 and Linux glibc x86_64/aarch64 (docs/ci.md).
    platforms = [
      "aarch64-darwin"
      "aarch64-linux"
      "x86_64-linux"
    ];
  };
})
