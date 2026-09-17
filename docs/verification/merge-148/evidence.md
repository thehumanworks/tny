# PR148 integration evidence

Initial state: local 2d710b6, remote feature 601caee, remote main cf423b8.
Local fixes are authorized for integration. Remote feature already includes main.
Existing hosted failures: Windows GCC LTO ICE (responses and runner),
Valgrind GCC internal-linkage warning, runtime mutation oracle mismatches,
Nix GCC array-bounds and Darwin SemVer/link failures.
Native goal omitted under higher-priority tool authorization rule.
Gate: INCOMPLETE.
