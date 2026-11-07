# NixOS module: `programs.tny` for every user (docs/nix.md). The Neovim
# setup goes to /etc/xdg/nvim/plugin, which Neovim loads for all users.
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
  options.programs.tny = removeAttrs (common.options cfg) [ "pyrightConfig" ];

  config = lib.mkIf cfg.enable {
    environment.systemPackages = out.packages;
    environment.variables = out.variables;
    environment.etc."xdg/nvim/plugin/tny-extensions.lua" = lib.mkIf cfg.neovim.enable {
      text = out.neovimLua;
    };
  };
}
