# Size and speed

## fx baseline

Source: [vercel-labs/fx README](https://github.com/vercel-labs/fx) (fetched 2026-08-18).

| Fact | Value |
| --- | --- |
| Language | Zig 0.16+ |
| Advertised binary | **7.8 MiB** |
| License | Apache-2.0 |
| Form | Unix-like CLI/shell, not a heavy TUI |
| Extra artifacts | `fx-core.wasm`, `fx-term.wasm` (embed path; not our v1) |
| Install | `curl -fsSL https://fx.sh/setup.sh \| bash` → `~/.local/bin/fx` |

Re-measure before claiming a win:

```bash
# after installing fx
stat -f%z "$(command -v fx)"          # macOS bytes
# or: wc -c "$(command -v fx)"
/usr/bin/time -p fx --version
/usr/bin/time -p fx ask --help
```

Record the fx version, sha256, and OS in the comparison table. Do not compare a debug tny against a ReleaseSafe fx.

## tny budgets

These apply to the **tny executable only**. `cursor-sdk-bridge` is a Bun-packaged host (see its `manifest.json` `runtime` field). Codex is a separate Rust binary. Neither counts.

| Build | Hard limit | Notes |
| --- | --- | --- |
| `tny` Release, stripped, macOS arm64 | 2.0 MiB | Fail CI if over |
| `tny` Release, stripped, Linux x64 (glibc dynamic) | 2.0 MiB | Same |
| `tny` musl static Linux (optional) | 3.0 MiB | TLS may force this higher; document if so |
| Stretch default | 1.0 MiB | Only if TLS stays dynamic |

Startup (empty `HOME` override, no network):

| Command | Budget |
| --- | --- |
| `tny --version` | 5 ms |
| `tny ask --help` | 5 ms |
| `tny` to first prompt (TUI, no spawn) | 10 ms |

Do not initialize backends until the user sends a turn or `ask` starts. `doctor` may spawn.

## How we stay under fx

1. C11, no C++ stdlib, no Zig runtime extras.
2. ANSI TUI, not a widget kit.
3. yyjson + wslay + nanopb, vendored as .c files you can see in `nm`.
4. System TLS, not a statically linked rustls/openssl.
5. Lazy backend load: Cursor/Codex/ACP code paths stay cold until selected.
6. No WASM, no bundled Node, no embedded Chromium.

## Measurement recipe (when code exists)

```bash
make release
strip build/tny
wc -c build/tny
hyperfine --warmup 3 './build/tny --version' 'fx --version'
```

Publish the table in the root README once numbers are real. Until then, treat 7.8 MiB and the budgets above as the contract.
