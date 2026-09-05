# Source filters shared by every tny derivation.
#
# Nix hashes the whole source tree, so an unfiltered `src = ../.` would rebuild
# the binary whenever docs/, site/, or .github/ changed. These filesets name
# exactly what each make target reads.
{ lib }:

let
  root = ../.;
  inherit (lib.fileset) toSource unions;

  # `make release`, `make lib-shared-active`, `make install`, and
  # `make install-lib-active`.
  # python/ is the pure-Python extension host that `make install` copies to
  # lib/tny/; libtny.pc.in and abi/ belong to the libtny install.
  buildFiles = unions [
    ../Makefile
    ../abi
    ../include
    ../libtny.pc.in
    ../python
    ../scripts/check_abi_baseline.py
    ../shell
    ../src
    ../third_party
  ];

  # `make test` additionally drives the fixture suites, the event-schema check,
  # the parser/help flag alignment check, and the conformance contract. Several
  # suites read the contract itself:
  # test_extension_contract.py against docs/features/, test_site.py by
  # regenerating site/ with scripts/site_build.py and diffing.
  # test_nix_ci_matrix.py reads the flake systems list, the nix workflow, and
  # this fileset so a filtered src cannot drop the files that test exists to
  # keep in lockstep.
  testFiles = unions [
    buildFiles
    ../.github/workflows/ci.yml
    ../.github/workflows/nix.yml
    # tests/integration/test_toolchain_pins.py keeps the mise pins and the CI
    # quality job on the same tool versions (docs/adr/0061).
    ../.mise.toml
    ../docs
    ../examples # tests/extensions/test_examples.py loads every shipped example
    ../flake.nix
    ../nix/source.nix
    ../scripts
    ../sdk/conformance
    ../sdk/schema
    ../site
    # All of tests/, which includes the frozen tool-profile A/B fixtures
    # under tests/bench/fixtures/tools/ that
    # tests/integration/test_bench_tools.py copies and scores (issue #103).
    ../tests
    # Speech fixtures (test_speech.py) generate their fake player in a temp
    # directory; no MP3 asset or host audio package enters the fileset.
    # Explicit contract for issue #88: every foreign MCP harness parser is
    # exercised from immutable fixture data inside the sandbox.
    ../tests/fixtures/mcp-import
    # Optional `make -C tnytty benchmark`: keep its product sources, helper,
    # and JSON runner available in the hermetic test source without running
    # the timing-sensitive benchmark as a routine Nix check.
    ../tnytty/Makefile
    ../tnytty/src
    ../tnytty/tests/bench
  ];
in
{
  build = toSource {
    inherit root;
    fileset = buildFiles;
  };

  tests = toSource {
    inherit root;
    fileset = testFiles;
  };
}
