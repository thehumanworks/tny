# libtny: the experimental ABI-0 headless embedding library (docs/libtny.md,
# ADR 0023) — shared library, public header, and pkg-config metadata.
#
# Usable without flakes:  pkgs.callPackage ./nix/libtny.nix { }
{
  lib,
  stdenv,
  openssl,
  pkg-config,

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
  nativeBuildInputs = [ pkg-config ]; # installCheck only

  enableParallelBuilding = true;
  dontConfigure = true;

  makeFlags = [
    "PREFIX=$(out)"
    "CC=${stdenv.cc.targetPrefix}cc"
    "TNY_VERSION=${finalAttrs.version}"
  ];
  buildFlags = [ "lib-shared" ];
  installTargets = [ "install-lib" ];

  # As in package.nix: TLS is dlopen'd, so the store path has to be in RUNPATH
  # and has to be added after `patchelf --shrink-rpath` has run.
  postFixup = lib.optionalString stdenv.hostPlatform.isLinux ''
    patchelf --add-rpath ${lib.getLib openssl}/lib $out/lib/libtny.so.0
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck

    test -f $out/include/tny/tny.h
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
    # ABI 0 supports macOS arm64 and Linux glibc x86_64/aarch64 (docs/ci.md).
    platforms = [
      "aarch64-darwin"
      "aarch64-linux"
      "x86_64-linux"
    ];
  };
})
