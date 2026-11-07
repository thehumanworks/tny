# nix-darwin module: `programs.tny` system-wide on macOS (docs/nix.md). The
# Neovim setup goes to /etc/xdg/nvim/plugin, on Neovim's default config path.
self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  common = import ./common.nix self { inherit lib pkgs; };
  cfg = config.programs.tny;
  out = common.values cfg;
in
{
  options.programs.tny = removeAttrs (common.options cfg) [
    "pyrightConfig"
    "skill"
  ];

  config = lib.mkIf cfg.enable {
    environment.systemPackages = out.packages;
    environment.variables = out.variables;
    environment.etc."xdg/nvim/plugin/tny-extensions.lua" = lib.mkIf cfg.neovim.enable {
      text = out.neovimLua;
    };
  };
}
