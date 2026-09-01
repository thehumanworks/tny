# Size and speed

## fx baseline

Measured 2026-08-18 from [fx.sh](https://fx.sh), the [README](https://github.com/vercel-labs/fx), and v0.0.3 release tarballs. fx is Zig 0.16, Apache-2.0, zero Zig package deps. It is **not** Bun/Node.

| Artifact | Size |
| --- | --- |
| Homepage claim | 6.39 MiB, “10 µs” cold start |
| README claim | 7.8 MiB (internal PGSO ceiling 7.800 MiB macOS arm64) |
| **v0.0.3 macOS arm64** | **6,748,416 B = 6.436 MiB** Mach-O |
| **v0.0.3 Linux x86_64** | **11,661,624 B = 11.12 MiB** static stripped ELF |
| `libfx` npm 0.0.3 unpacked | 34.8 MiB (WASM/NAPI — not in tny) |
| CI CLI budget | **2.000 ms mean** on Linux for `fx`, `help`, `status --json`, … |

The “10 µs” number is the `FX_BENCH=1` path (parse argv, exit before TTY). Do not publish a 10 µs claim. Beat **measured** `exec` + first paint, and beat **6.436 MiB macOS / 11.12 MiB static Linux**.

Re-measure the same fx version you compare against. Do not compare debug tny to ReleaseSafe fx.

## tny budgets

These apply to the **tny executable only**. `cursor-sdk-bridge` is a Bun-packaged host (see its `manifest.json` `runtime` field). Codex is a separate Rust binary. Neither counts.

| Build | Must | Stretch |
| --- | --- | --- |
| macOS arm64, stripped, libSystem + Security.framework | **< 1.8 MiB** | < 1.2 MiB |
| Linux musl static, stripped | **< 1.5 MiB** | < 1.0 MiB |
| Linux glibc dynamic | **< 1.0 MiB** ([ADR 0053](adr/0053-forked-turn-isolation.md): isolation without tmux keeps this budget hard) | < 0.8 MiB |
| Windows x86_64 (MSYS-linked exe) | **< 2.0 MiB** | — |
| wasm artifact, js glue + `.wasm`, Asyncify included ([ADR 0017](adr/0017-wasm-browser-parity.md)) | **< 1.5 MiB** | < 1.0 MiB |
| Idle RSS after prompt | **< 4 MiB** | < 2 MiB |

Those still beat fx by ~3–4× on macOS and ~7× on static Linux. The `ci`
workflow runs `make size-check` on every target (and `make wasm-size-check`
for the wasm artifact) and fails the PR if the budget is exceeded
([ci.md](ci.md)). Current wasm artifact: ~0.66 MiB total with broad
Asyncify instrumentation — no narrowing needed yet.

Startup (empty `HOME` override, no network):

| Command | Must | Stretch |
| --- | --- | --- |
| `tny --version` / `tny ask --help` | **< 5 ms** median | < 2 ms (match fx’s 2 ms Linux CLI gate if we can) |
| TUI first prompt (no spawn) | **< 10 ms** | < 5 ms |

Do not initialize backends until the user sends a turn or `ask` starts. Human
`doctor` may spawn bounded health probes; `doctor --json` is a side-effect-free
configuration/capability query and never starts a provider or Python.

Packaged builds pay the budget too. The Nix package
([ADR 0035](adr/0035-nix-flake-packaging.md)) runs `make size-check` in its
`checkPhase` and adds a `makeBinaryWrapper` — a compiled wrapper, not a shell
script — for `python3` and the CA bundle, measured at ~0.3 ms on Linux x86_64
(0.73 ms wrapped vs 0.42 ms unwrapped). A shell wrapper would cost several
times that; `packages.tny-unwrapped` skips it entirely.

## How we stay under fx

1. C11, no C++ stdlib, no Zig runtime extras.
2. ANSI TUI, not a widget kit.
3. yyjson + picohttpparser + wslay, vendored as .c files you can see in `nm`.
   (nanopb deferred: v1 speaks Connect with the JSON codec, no protobuf runtime.)
4. System TLS, **dlopen'd at first TLS use**: macOS Security.framework (eager
   framework linking costs ~1.2 ms per launch and loses the startup race),
   Linux the distro's `libssl.so.3`/`.so.1.1`
   ([adr/0007](adr/0007-linux-tls-system-openssl.md), +4 KiB total, `ldd`
   stays libssl-free). Never static or vendored OpenSSL. musl static builds
   cannot dlopen: plain http works, https errors cleanly there.
5. Lazy backend load: Cursor/Codex/ACP stay cold until selected. No upgrade/MCP/skill walk before first prompt.
6. No WASM, NAPI, sounds, or bundled Node in the default CLI.

## Measurement recipe (when code exists)

```bash
make release
strip build/tny
wc -c build/tny
hyperfine --warmup 3 './build/tny --version' 'fx --version'
```

Publish the table in the root README once numbers are real. Until then, beat **6.436 MiB macOS / 11.12 MiB static Linux** and the budgets above. Do not UPX.

## SDK event-schema foundation (ABI 0.3)

The public event-schema/view work is required to remain effectively free on
CLI startup because the default executable does not call the public ABI. On the
same macOS arm64 host, comparing parent commit `471885e` with this worktree:

| Metric | parent | ABI 0.3 worktree | delta |
| --- | ---: | ---: | ---: |
| stripped `tny` | 579,152 B | 579,152 B | 0 B |
| `libtny.0.dylib` | 372,016 B | 355,664 B | -16,352 B |
| `tny --version` median, `hyperfine -N`, 100 runs | 1.833 ms | 1.837 ms | +0.004 ms |
| `tny --version` mean | 1.852 ms | 1.855 ms | +0.003 ms |

The dylib reduction is not attributed to the feature: the new ABI adds two
exports, so the smaller link result is treated as toolchain/dead-strip layout
variance rather than an optimisation claim. The relevant gate is that CLI size
and startup did not regress measurably.
