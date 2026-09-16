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
| Linux glibc dynamic, x86_64 and other architectures | **≤ 1,048,576 bytes** | < 0.8 MiB |
| Linux glibc dynamic, aarch64/arm64 | **≤ 1,052,672 bytes** ([ADR 0120](adr/0120-measured-linux-aarch64-cpp-artifact-budget.md): measured 4 KiB allowance) | ≤ 1 MiB |
| Windows x86_64 (MSYS-linked exe) | **< 2.0 MiB** | — |
| wasm artifact, js glue + `.wasm`, Asyncify included ([ADR 0017](adr/0017-wasm-browser-parity.md)) | **< 1.5 MiB** | < 1.0 MiB |
| Idle RSS after prompt | **< 4 MiB** | < 2 MiB |

The fx figures above are historical, not a current comparison. The `ci`
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

On aarch64 Linux the budget hides a cliff ([ADR 0111](adr/0111-aarch64-size-cliff.md)):
the two `LOAD` segments are aligned to 64 KiB and the RELRO end must sit
on a 64 KiB boundary, so the file grows by a whole 64 KiB the moment the
read-only (`R E`) segment passes ≈ 975 KiB (`64 KiB − relro_size` past a
boundary; `readelf -lW build/tny` shows the segment). Read a sudden +64 KiB
as that cliff, not as 64 KiB of new instructions. The measured private-C++
migration uses the frozen, architecture-specific allowance in ADR 0120;
all other caps and the automated checks remain unchanged. The Linux native
lanes already omit the frame pointer and drop dead yyjson paths for margin.

Packaged builds pay the budget too. The Nix package
([ADR 0035](adr/0035-nix-flake-packaging.md)) runs `make size-check` in its
`checkPhase` and checks the installed payload against the same Makefile-owned
budget in `installCheckPhase` ([ADR 0103](adr/0103-nix-link-time-runtime-path.md)).
The wrapped variant measures the real `.tny-wrapped` payload. It adds a
`makeBinaryWrapper` — a compiled wrapper, not a shell
script — for `python3` and the CA bundle, measured at ~0.3 ms on Linux x86_64
(0.73 ms wrapped vs 0.42 ms unwrapped). A shell wrapper would cost several
times that; `packages.tny-unwrapped` skips it entirely.

## How we stay under fx

1. C11 with scoped private C++20 owners (ADR 0114); measure C++ runtime dependencies and artifact deltas. No Zig runtime extras.
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

## C++ ownership series: reproducible startup gate

The private ownership migration (ADR 0114, issues #137–#139) keeps the size
ceilings above unless a separately measured policy amendment justifies a
revision. Report `otool -L` / `ldd` dependencies alongside stripped bytes;
a dynamically loaded C++ runtime is not part of the executable's byte count.
Do not attribute a language change's size or speed effect without measurement.

Use an idle reference host, identical toolchain/release flags, and immutable
baseline/candidate binaries. The startup runner creates a new empty HOME and
workspace for every launch, submits no turn, and selects the lazy native
OpenAI provider. It detects the completed PTY composer paint, not the banner,
raw-mode setup, or first token. It drains the PTY during fixture shutdown.

```sh
python3 tests/bench/bench_startup.py \
  --baseline /absolute/pre-series/build/tny \
  --candidate /absolute/candidate/build/tny \
  --output /absolute/evidence/startup.json
```

Defaults provide 102 samples of `--version` and `ask --help` for each binary,
in three paired batches with alternating artifact order, and 20 fresh PTY
launches for each binary. The JSON retains every sample, median/p95,
artifact SHA-256 and size, host identity, configuration, and pass/fail status.
The command returns nonzero on an absolute or relative gate failure, artifact
mutation, timeout, or unexpected process failure. CLI median must be below
5 ms and added median at most max(0.25 ms, 10%); prompt median must be below
10 ms and added median at most max(0.5 ms, 10%). Always compare the final
combined implementation to the pre-series baseline too.

`tests/integration/test_bench_startup.py` checks threshold arithmetic,
fragmented-paint discrimination, sampling, and environment isolation without
noisy timing assertions in CI. Run the existing local-mock `bench_ttft.py`
`tui` and `ask-stdin` modes separately with 20 iterations per artifact.
Those measure a different boundary and do not replace first-prompt evidence.

The parser corpus microbenchmark builds the same C driver against baseline
and candidate source trees, selecting `.c` or `.cpp` implementations without
compiling untouched C as C++. It obtains release flags from each Makefile,
uses the existing tny allocation boundary in both builds, and links the C-only
baseline without an artificial C++ runtime dependency. It covers SSE (CRLF,
comments, multiline data, UTF-8 and EOF flush), Connect frames/keepalives/end
trailers, and 32 id-first tool-call assemblies with reused wire indices.

```sh
python3 tests/bench/bench_parsers.py \
  --baseline /absolute/pre-series \
  --candidate /absolute/candidate \
  --work-dir /absolute/new-evidence-directory
```

Whole, one-byte and deterministically fragmented inputs must produce identical
per-corpus observations. The runner keeps three alternating baseline/candidate
batches, raw output checksums, time, allocation counts, peak RSS, source and
artifact hashes, compiler identities, build commands and dynamic dependencies.
Both median elapsed time and median peak RSS may increase by at most 10%.
The default is 2,000 fresh parser lifetimes per sample; increase iterations
on fast hosts rather than interpreting timer noise as an improvement.
`--iterations 2` is a functional smoke only, never performance acceptance.
The corpus driver is tested on pre-migration C as well as private C++.

Allocation instrumentation is part of this controlled parser comparison;
report normal CLI startup independently. Peak RSS includes process/runtime
costs and corpus storage, so preserve the raw values and dependency inventory.
The deterministic `test_bench_parsers.py` checks reject semantic differences,
missing samples and invalid measurements before computing performance ratios.

For the event migration, use the same build/provenance/comparison machinery
with the real private engine and a fixed synchronous callback source:

```sh
python3 tests/bench/bench_events.py \
  --baseline /absolute/pre-change \
  --candidate /absolute/candidate \
  --work-dir /absolute/new-event-evidence-directory
```

Each iteration emits 64 events and a duplicated terminal callback, immediately
overwrites every borrowed payload buffer, then drains and releases the queue.
The driver checks embedded-NUL text lengths, all retained string fields,
monotonic event ordering, exactly one terminal, and unchanged logical payload
accounting. It exercises real engine admission, copying and release, not a
standalone owner substitute. Inputs and callback observers are identical across
builds. Use 2,000 iterations for measurement; a two-iteration smoke only proves
the executable harness and behavioral oracles. Allocation counts and process
peak RSS supplement, rather than replace, the logical queue-byte counters.
