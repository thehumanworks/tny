{
  description = "tny — a tiny C11 TUI + CLI coding-agent harness";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;

      # No x86_64-darwin: Intel Mac is not a tny target (docs/ci.md), and
      # nixpkgs has dropped the platform.
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];

      forAllSystems =
        f:
        lib.genAttrs systems (
          system:
          f {
            inherit system;
            pkgs = nixpkgs.legacyPackages.${system};
          }
        );

      # ADR 0014: the git tag is the single source of truth for the version and
      # no version string is committed. A flake source tree has no .git, so
      # `git describe` cannot run — exactly the git-less case the ADR points at
      # TNY_VERSION for. The flake revision is the equivalent identity, and
      # matches what `git describe --tags --always --dirty` prints when no tag
      # is reachable. To stamp a release version instead:
      #   pkgs.tny.override { version = "0.2.1"; }
      version = self.shortRev or self.dirtyShortRev or "0.0.0-unknown";
    in
    {
      # For consumers who already compose an overlay list:
      #   nixpkgs.overlays = [ tny.overlays.default ];  then use pkgs.tny
      overlays.default = final: _prev: {
        tny = final.callPackage ./nix/package.nix { inherit version; };
        libtny = final.callPackage ./nix/libtny.nix { inherit version; };
      };

      packages = forAllSystems (
        { pkgs, ... }:
        let
          tny = pkgs.callPackage ./nix/package.nix { inherit version; };
        in
        {
          inherit tny;
          default = tny;

          # No PATH/CA wrapper: the binary runs exactly as built, at the cost
          # of finding python3 and the CA bundle in the caller's environment.
          tny-unwrapped = tny.override { wrapRuntime = false; };

          libtny = pkgs.callPackage ./nix/libtny.nix { inherit version; };
        }
      );

      apps = forAllSystems (
        { system, ... }:
        let
          tny = {
            type = "app";
            program = lib.getExe self.packages.${system}.tny;
            meta.description = "Run the tny TUI/CLI";
          };
        in
        {
          inherit tny;
          default = tny;
        }
      );

      devShells = forAllSystems (
        { pkgs, ... }:
        {
          default = pkgs.callPackage ./nix/devshell.nix { };
        }
      );

      checks = forAllSystems (
        { pkgs, system }:
        {
          # tny itself carries the size-budget gate in its checkPhase.
          inherit (self.packages.${system}) tny tny-unwrapped;

          tests = pkgs.callPackage ./nix/tests.nix { inherit version; };
        }
        // lib.optionalAttrs (lib.elem system self.packages.${system}.libtny.meta.platforms) {
          inherit (self.packages.${system}) libtny;
        }
      );

      formatter = forAllSystems ({ pkgs, ... }: pkgs.nixfmt-tree);
    };
}
