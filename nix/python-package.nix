# The `tny` Python package (sdk/python): the native SDK plus the tool-extension
# API (tny.tool, tny.group, tny.proxy, ...) with py.typed, so editors and type
# checkers resolve `import tny` in extension code (docs/extensions.md).
#
# Pure Python. The extension API needs nothing else; the SDK's Runtime and
# Toolkit load libtny at run time from TNY_LIBRARY_PATH (the Home Manager and
# NixOS modules set it where libtny builds).
#
# Usable without flakes:  python3Packages.callPackage ./nix/python-package.nix { }
{
  lib,
  buildPythonPackage,
  setuptools,
  wheel,
  cffi,
  version ? "0.0.0.dev0",
}:

buildPythonPackage {
  pname = "tny";
  inherit version;
  pyproject = true;

  # sdk/python/src/tny/{_registry,_schema,_mcp}.py are symlinks into
  # python/tny, so both trees travel together.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../sdk/python
      ../python/tny
    ];
  };
  sourceRoot = "source/sdk/python";

  build-system = [
    setuptools
    wheel
  ];
  dependencies = [ cffi ];

  pythonImportsCheck = [ "tny" ];

  meta = {
    description = "tny Python SDK and tool-extension API";
    homepage = "https://github.com/thehumanworks/tny";
    platforms = lib.platforms.unix;
  };
}
