# libtny cross-language conformance v1

`v1.json` is the single executable lifecycle, event, ownership, misuse, and
transport contract for every supported SDK and artifact. Package lanes provide
a small adapter command; they do not copy scenario definitions or decide their
own release status. See [protocol.md](protocol.md) for the JSON protocol.

Build and certify the native shared library with the reference adapter:

```sh
make lib-shared debug
python3 sdk/conformance/run.py \
  --artifact build/lib/libtny.0.dylib \
  --report build/conformance/c-reference.json \
  -- python3 sdk/conformance/adapters/c_reference.py
```

Use `build/lib/libtny.so.0` on Linux. The reference adapter drives live ABI 0.5
turns against the strict local OpenAI mock, reopens a persisted session, checks
allow/deny/stale permission handling, cancellation and auth errors, and invokes
the public steer/cancel path to prove rejected text is returned immediately
before an interrupted terminal and survives a close/reopen boundary. It also
invokes the existing focused C fixtures for queue backpressure and
`chunked_survives_every_split_boundary`. A compiled reference probe feeds an
unknown internal event through the public event reader and verifies that its
numeric kind and payload survive without aliasing a known event constant.

Release policy
--------------

A release report is valid only when:

- the orchestrator launched the adapter and independently verified the artifact
  SHA-256, ABI, platform, transport, capability snapshot, scenario evidence,
  ordered event transcript, terminal count, and secret-safety rules;
- every scenario applicable to advertised capabilities passed with every v1
  assertion ID and evidence from a successful execution;
- no applicable release-required scenario is missing, failed, `not_run`, or
  `unsupported`; and
- unavailable capabilities are explicit, so unsupported wasm/static/DLL or
  provider behavior is never presented as parity.

Reports are deterministic: volatile timestamps and opaque IDs are validated,
then normalized to ordinals and presence booleans. Adapter stdout and stderr are
never reflected on failure, avoiding accidental credential disclosure.

Contract and negative tests:

```sh
python3 sdk/conformance/check.py
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s tests/conformance -p 'test_*.py' -v
```

The negative fixtures prove that broken event ordering, contradictory capability
claims, wrong artifact hashes, forbidden/secret fields, missing scenarios, and
`not_run`/`unsupported` applicable scenarios all block release.
