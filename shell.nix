# `nix-shell` for people not using flakes; `nix develop` uses the same file.
{
  pkgs ? import <nixpkgs> { },
}:

pkgs.callPackage ./nix/devshell.nix { }
