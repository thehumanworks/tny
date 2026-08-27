# libtny ABI compatibility

The native ABI1.0 baseline is active. Its machine-readable state is `active`
in [`abi/baseline-v1.json`](../abi/baseline-v1.json). Activation followed
immutable Linux x86_64/aarch64 consumer verification and the complete
supported-platform, conformance, sanitizer, fuzz, SDK, and packaging matrices.
The final ABI0.8 compatibility artifact and header are built from exact commit
`510a95c` and verified against
[`abi/compat0.json`](../abi/compat0.json).

## Compatibility promises

| Change | Patch | Minor | New ABI major |
| --- | --- | --- | --- |
| Bug/security fix preserving symbols, layouts, constants, ownership, and ordering | yes | yes | no |
| Add symbol in a new inherited ELF `LIBTNY_1.N` node | no | yes | no |
| Append a sized struct field beyond its frozen boundary | no | yes | no |
| Add an event/provider/stop value or capability bit | no | yes | no |
| Move/retype/remove a field or symbol; change a numeric value or calling convention | no | no | yes |
| Add Windows/static/musl-shared/wasm packaging | separate platform review | separate platform review | only if the C ABI changes |

ABI compatibility covers loadability and the documented ownership/event
semantics. It does not promise that an optional capability is enabled. Query
`tny_runtime_get_capabilities*` and ignore unknown mask bits. Preserve unknown
numeric event, provider, stop, diagnostic, and error values; reject unknown
caller-supplied permission decisions.

## Sized records

ABI-1 callers provide allocation capacity to every canonical initializer,
read, query, and create call and check its status. The library records that
capacity in `struct_size`, validates a whole-field boundary and the
operation-specific `minimum_size`, and touches only the shared prefix. A newer
library supplies defaults for fields absent from an older caller. An older
library ignores a newer caller's tail. Existing fields and the baseline
reserved tail never move or acquire new meaning; additions begin at the
baseline `append_from` offset.

Capacities below the frozen size are accepted only when they end exactly at a
documented field boundary. A size ending inside a pointer, byte view, scalar,
or reserved array is rejected before any input field is read or output field
is written. Capacities greater than the frozen size are allowed; the unknown
tail is neither read nor cleared. Nonzero reserved bytes are ignored in ABI1.

This differs deliberately from ABI0.8, whose one-argument initializers and
implicit-capacity operations cannot safely grow their destination. Those
signatures are absent from major 1. Recompile and pass `sizeof(caller_record)`.

## Platform artifacts

- glibc Linux x86_64/aarch64: SONAME `libtny.so.1`, initial symbol node
  `LIBTNY_1.0`, later additions in inherited `LIBTNY_1.N` nodes.
- macOS arm64: install name `@rpath/libtny.1.dylib`, compatibility version
  `1.0.0`, monotonically increasing numeric current version.
- Windows DLL, static archives, musl shared libraries, and wasm embedding:
  unsupported until separately designed and tested.

An unversioned `libtny.so` is a link-time symlink, not a runtime lookup target.
SDK packages load only the versioned library shipped for their ABI major.

## ABI 0 transition

The ABI-1 release includes final ABI-0 shared artifacts and the versioned
header from `510a95c`. `libtny-0.pc` points only at the ABI0 include root;
`libtny.pc` points only at ABI1. ABI 0 receives only security and critical data-loss fixes until
the later of 180 days or product release 1.2.0. Both majors are side-by-side
installable. ABI-1 deprecations retain a replacement for at least 12 months and
two minor releases; removal waits for ABI 2.

Commit `510a95c` and its archive hash are permanent initial-GA provenance.
An ABI0 security fix uses an appended reviewed, hash-pinned maintenance commit
in `abi/compat0.json`; it may not change the frozen header, layouts, signatures,
exports, artifact identity, or old-consumer results.

## Automated evidence

Validate the proposed baseline itself:

```sh
python3 scripts/check_abi_baseline.py
```

Compare a generated implementation snapshot:

```sh
python3 scripts/check_abi_baseline.py \
  --candidate build/abi/libtny-v1-current.json
```

The candidate is produced from a compiled C layout/constant probe, exact `nm`
or `readelf` exports and version nodes, `readelf -d` SONAME, and `otool -D/-L`
plus Mach-O version metadata. The checker rejects changed constants, missing
symbols, platform identity drift, fixed-layout changes, prefix movement, and
new fields inserted before an append boundary.

The compatibility job keeps immutable old-consumer binaries from ABI 1.0 and
every supported minor release, with hashes and build metadata. It runs them
against the current library. Checked-in minimum/current source fixtures compile
against both supported headers. The repository also keeps deliberate negative
fixtures; a compatibility job which does not reject them is itself failing.

ABI 1 was not activated on schema checks alone. Full conformance, sanitizers,
fault injection, fuzzing, focused mutation, TLS, native platform, size/startup,
SDK, and clean-package gates passed before activation and remain
release-blocking.
