# Non-flake entry point, for channels and `nix-build`.
#
#   nix-build -A tny            # build/tny + the Python extension host
#   nix-build -A libtny         # the active ABI-1 embedding library
#
# Flake users want flake.nix instead; both call the same nix/*.nix files.
{
  pkgs ? import <nixpkgs> { },

  # Without git (see ADR 0014) the build cannot derive its own version.
  # `nix-build -A tny --argstr version 0.2.1` stamps a release.
  version ? "0.0.0-unknown",
}:

{
  tny = pkgs.callPackage ./nix/package.nix { inherit version; };
  libtny = pkgs.callPackage ./nix/libtny.nix { inherit version; };
}
