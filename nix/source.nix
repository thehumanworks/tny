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

  # test_windows_lto_flags.py inspects this same Makefile's MSYS annotation
  # override while retaining LTO and -Werror; it needs no platform SDK.
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
    # Image fixtures (test_image_service.py, and test_image_workflow.py for the
    # #122 dimension/strict-size cases) embed or generate their PNG/JPEG/WebP
    # bytes in source with stdlib zlib, and create reference/output files in a
    # temporary directory; no external assets or image libraries.
    # test_image_input.py adds the image-input capability gates: a throwaway
    # HOME with its own settings.json, fake credentials, the stdlib loopback
    # provider and tests/integration/fake_acp_agent.py, all already in ../tests.
    # test_image_exports.py (#125) adds no source inputs either — its images
    # are generated with stdlib struct/zlib and decoded the same way — but it
    # does drive the optional ImageMagick 7 `magick` executable, which
    # nix/tests.nix declares as a test-only dependency. Nothing tny builds or
    # installs links or requires it.
    # test_settings_schema.py reads schemas/settings.schema.json, named below,
    # and skips its optional jsonschema case when that library is absent; the
    # sandbox adds no new Python dependency for it.
    # test_job_artifacts.py (ADR 0098) uses the same stdlib loopback providers
    # and actual detached jobs. Its two fixtures in tests/fixtures compile the
    # real core with the existing make/compiler/debug objects; no added package.
    # test_image_preview_workflow.py (ADR 0097) imports test_image_workflow.py,
    # already in the integration inventory. All artifacts/HTTP fixtures are
    # generated in a private HOME with Python stdlib. Real optional ImageMagick
    # uses the existing export dependency; no new executable or input directory.
    # test_image_preview_queue.py (ADR 0096) adds no input either: it generates
    # its PNGs with stdlib struct/zlib, writes its throwaway control client and
    # settings.json into a temporary HOME, and talks to the real runner's
    # AF_UNIX socket with stdlib socket/json; ../tests already includes it.
    # Its generated no-socket helper check compiles src/cli/cmd_control.c and
    # existing headers (all in ../src); the fake SSH fixture adds no source.
    # The #127 manifest/replay cases in test_image_workflow.py (-k Manifest) and
    # test_image_service.py add no inputs either: records are JSON the binary
    # writes into a throwaway HOME, hashes come from stdlib hashlib, and the
    # writer-guard and killed-writer cases use only os/subprocess/threading.
    # tests/fixtures/toolkit_provider.py supplies stdlib-only HTTP/media data
    # to native ABI and SDK toolkit tests; ../tests already includes it.
    # The canonical ask-event suites (test_ask_events.py, its libtny
    # reader-conformance companion, and tests/abi/test_event_jsonl.py, ADR
    # 0090) add no asset: they run a stdlib loopback provider, a fake MCP
    # server written into a temporary HOME, and read sdk/schema/events.json
    # plus include/tny/tny.h, all already in this fileset.
    # tests/fixtures/jobs_launch_barrier.c is compiled as a local pipe-failure
    # and snapshot-commit interposer by test_jobs.py; ../tests includes it,
    # stdenv supplies cc, and the host C runtime supplies dl for dlsym.
    # test_jobs_cleanup_hold.py uses the same stdlib provider and existing
    # fcntl locks. Native Windows-only test_jobs_msys.py and its two C fixtures
    # (jobs_msys_scope.c/jobs_msys_tree.c) are included by ../tests and skip
    # outside MSYS2; no guest image or generated executable is an input.
    # The durable-jobs suite (test_jobs.py, ADR 0093) adds no media asset: it
    # runs the built tny against a stdlib loopback provider under a throwaway
    # HOME, generates its own PNG bytes with zlib, and observes real detached
    # children through `ps` — which nix/tests.nix already declares for the TUI
    # and ask-event suites.
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
    # The published settings schema, read by tests/integration/test_settings_schema.py.
    ../schemas
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

  # Installed CLI TLS checks use only the maintained HTTPS driver and its
  # stdlib provider; keep unrelated tests out of the package source hash.
  packageChecks = toSource {
    inherit root;
    fileset = unions [
      ../tests/integration/test_https.py
      ../tests/integration/mock_openai.py
    ];
  };

  tests = toSource {
    inherit root;
    fileset = testFiles;
  };
}
