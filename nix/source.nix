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
    ../shell # installed workflows and the Zsh quick-ask widget
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
    ../tests # includes quick-ask PTY, cache-routing fixtures, and optional cache benchmark
    # test_worktree.py generates temporary Git repositories and imports the
    # shared test_tui.py PTY harness; both are included by ../tests above.
    # Image fixtures (test_image_service.py) embed their PNG in source and
    # create reference/output files in a temporary directory; no external assets.
    # tests/fixtures/toolkit_provider.py supplies stdlib-only HTTP/media data
    # to native ABI and SDK toolkit tests; ../tests already includes it.
    # Speech fixtures (test_speech.py) generate their fake player in a temp
    # directory; no MP3 asset or host audio package enters the fileset.
    # make dictation-fixture/test-dictation reuse the same src/ and stdlib
    # fixtures, with fake xAI/Grok credentials and a test-only loopback URL.
    # Dictation fixtures generate PCM WAV bytes and fake microphone recorders
    # in temporary directories; no microphone or external media asset is needed.
    # Prompt optimisation fixtures create a nested project and loopback HTTP
    # server, reusing test_tui.py; all inputs are covered by ../tests above.
    # test_interrupt.py compiles fixtures/slow_lock.c on Darwin to exercise
    # elapsed stop deadlines under slow lock probes; ../tests includes it.
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
