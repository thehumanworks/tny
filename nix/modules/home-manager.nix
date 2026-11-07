# Home Manager module: `programs.tny` (docs/nix.md). Works on NixOS (as a
# Home Manager NixOS module), on macOS (standalone or via nix-darwin), and on
# any Linux.
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
  options.programs.tny = common.options cfg;

  config = lib.mkIf cfg.enable {
    home.packages = out.packages;
    home.sessionVariables = out.variables;
    home.file.".tny/extensions/tools/pyrightconfig.json" = lib.mkIf cfg.pyrightConfig {
      text = out.pyrightConfig;
    };
    home.file.".tny/skills/tny-tool-extensions/SKILL.md" = lib.mkIf cfg.skill {
      source = ../../skills/tny-tool-extensions/SKILL.md;
    };
    xdg.configFile."nvim/plugin/tny-extensions.lua" = lib.mkIf cfg.neovim.enable {
      text = out.neovimLua;
    };
  };
}
