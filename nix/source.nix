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
  testFiles = unions [
    buildFiles
    ../docs
    ../examples # tests/extensions/test_examples.py loads every shipped example
    ../scripts
    ../sdk/conformance
    ../sdk/schema
    ../site
    ../tests
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
