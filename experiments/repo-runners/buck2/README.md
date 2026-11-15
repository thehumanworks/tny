# Buck2 tny source-build experiment

This is a real Buck2 build action around tny's existing Make build. Buck2 declares the source archive, build script and output binary, executes the action locally, caches its output and runs the result. It does not replace the internal Make dependency graph with native Buck C/C++ rules.

## Reproduce

Install Buck2, a C11/C++20 compiler, a native SDK/libc development environment, Make, Python, Perl, curl, tar/xz and the utilities used by the existing build. This experiment does not provision those tools itself. Tests used the official 2026-11-15 Buck2 macOS ARM64 and Linux x86_64 release assets. The macOS compiler/SDK were already installed. Nix supplied the compiler and utilities on the NixOS test host; it did not build tny. No FHS compatibility environment was needed for these Buck2 trials.

```sh
cd experiments/repo-runners/buck2
sh prepare.sh
buck2 run --config runner.path="$PATH" //:tny -- --version
```

`prepare.sh` archives the selected checkout's committed HEAD, excludes untracked/generated build outputs, and preserves an unchanged archive. Uncommitted application changes are intentionally excluded. `runner.cc` and `runner.cxx` can select compiler drivers; defaults are cc and c++. `runner.path` supplies their tool environment. Changing these configuration strings invalidates the action, but replacing a compiler in place at the same path is not tracked. Clean/rebuild after such toolchain changes.

The action builds in a fresh temporary directory, then exports only the executable. CPython and its pinned native-library source archives are downloaded and verified by tny's existing script. This network access and reliance on host tools mean the action is not hermetic, and it is deliberately local-only. No GitHub Actions runners or published tny binaries are used.

The generic experimental GitHub adapter is in the mmr branch: `buck-repo-run` plus sibling `repo-run`. It accepts `--project-dir experiments/repo-runners/buck2`, repeated `--config` arguments and a pinned GitHub repository reference. `-- tny --version` maps to Buck target `//:tny`. It executes `prepare.sh` and preserves the caller's runtime working directory.

## Verdict

Buck2 can orchestrate this existing Make/CPython build on both operating systems once a recipe and compiler environment are supplied. That is useful build execution and caching, but does not automatically solve arbitrary GitHub repository packaging, compiler/SDK provisioning or cross-compilation. A fully declared toolchain/dependency integration would require more work than this local adapter.

The recipe stamps version0.39.0 for this evaluated snapshot. Future application revisions need explicit version handling; version output alone does not establish source identity.

macOS initial build used commit57298966 and passed --version plus8 fixture runtime tests. The quality gate passed after formatting/import-order fixes to the existing experimental smoke script: C/C++ tidy and strict-warning checks passed in the first run; the resumed quality run reused those unchanged inputs and passed all remaining checks. Logs are under /Users/tomas/repo-runner-evaluation/logs/tny-quality-buck2*.
