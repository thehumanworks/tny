# Size and speed

Current policy: keep tny fast, portable and small through measurement. There
is **no** binary-size ceiling and **no** product goal to beat fx on artifact
size ([ADR 0150](adr/0150-agent-first-harness-and-measured-footprint.md)).
Startup, TTFT, leak, ABI and payload bounds remain active.

Report stripped bytes (`wc -c`) and runtime dependencies (`otool -L` / `ldd`)
on release builds. Host binaries (`cursor-sdk-bridge`, ACP agents) stay
external and are not part of the tny artifact. C++ runtime libraries are
reported separately from the executable.

## Historical fx baseline (not a product goal)

Measured 2026-08-18 from [fx.sh](https://fx.sh), the
[README](https://github.com/vercel-labs/fx), and v0.0.3 release tarballs. fx
is Zig 0.16, Apache-2.0, zero Zig package deps. It is **not** Bun/Node. These
rows are dated bake-off evidence. They do not define tny's mission.

| Artifact | Size |
| --- | --- |
| Homepage claim | 6.39 MiB, “10 µs” cold start |
| README claim | 7.8 MiB (internal PGSO ceiling 7.800 MiB macOS arm64) |
| **v0.0.3 macOS arm64** | **6,748,416 B = 6.436 MiB** Mach-O |
| **v0.0.3 Linux x86_64** | **11,661,624 B = 11.12 MiB** static stripped ELF |
| `libfx` npm 0.0.3 unpacked | 34.8 MiB (WASM/NAPI — not in tny) |
| CI CLI budget (fx's, historical) | **2.000 ms mean** on Linux for `fx`, `help`, `status --json`, … |

The “10 µs” number is the `FX_BENCH=1` path (parse argv, exit before TTY). Do
not publish a 10 µs claim. Re-measure the same fx version if you compare
against it. Do not compare debug tny to ReleaseSafe fx. Former tny
platform ceilings (1 MiB / 1.5 MiB / 1.8 MiB / decimal 6 MB) are likewise
historical; ADR 0121 and ADR 0120 record those policies and their
measurements.

## Speed budgets

Startup (empty `HOME` override, no network):

| Command | Must | Stretch |
| --- | --- | --- |
| `tny --version` / `tny ask --help` | **< 5 ms** median | < 2 ms |
| TUI first prompt (no spawn) | **< 10 ms** | < 5 ms |

Do not initialize backends until the user sends a turn or `ask` starts. Human
`doctor` may spawn bounded health probes; `doctor --json` is a side-effect-free
configuration/capability query and never starts a provider or Python.

On aarch64 Linux a file-size jump can be a linker cliff, not new code
([ADR 0111](adr/0111-aarch64-size-cliff.md)): the two `LOAD` segments are
aligned to 64 KiB and the RELRO end must sit on a 64 KiB boundary, so the
file grows by a whole 64 KiB the moment the read-only (`R E`) segment
passes ≈ 975 KiB (`64 KiB − relro_size` past a boundary; `readelf -lW
build/tny` shows the segment). Read a sudden +64 KiB as that cliff, not as
64 KiB of new instructions. The Linux native lanes omit the frame pointer
and drop dead yyjson paths for startup and layout reasons, not a size gate.

The Nix package ([ADR 0035](adr/0035-nix-flake-packaging.md)) still builds
through the Makefile. Installed payload measurement and the compiled
`makeBinaryWrapper` (~0.3 ms on Linux x86_64: 0.73 ms wrapped vs 0.42 ms
unwrapped) remain; they are not a byte ceiling. `packages.tny-unwrapped`
skips the wrapper.

## How we stay small and fast

1. C11 with scoped private C++20 owners (ADR 0114); measure C++ runtime
   dependencies and artifact deltas. No Zig runtime extras.
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
6. No NAPI, sounds, or bundled Node in the default CLI. wasm is the landing
   terminal ([ADR 0017](adr/0017-wasm-browser-parity.md)), not a second
   agent loop.

Do not UPX. Do not weaken ownership, error handling or cleanup to shave bytes.

## Measurement recipe

```bash
make release
strip build/tny
wc -c build/tny
hyperfine --warmup 3 './build/tny --version'
```

Publish dated tables when numbers are real. Same-host before/after numbers
are required for performance claims.

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

The private ownership migration (ADR 0114, issues #137–#139) reports `otool
-L` / `ldd` dependencies alongside stripped bytes; a dynamically loaded C++
runtime is not part of the executable's byte count. Do not attribute a
language change's size or speed effect without measurement. There is no
artifact-size ceiling to keep ([ADR 0150](adr/0150-agent-first-harness-and-measured-footprint.md)).

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
