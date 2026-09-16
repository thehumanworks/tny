# Startup: pre-series-1d8ad71

Policy: 0115-v1; host: macOS-27.0-arm64-arm-64bit-Mach-O
Build metadata: Fresh release build from read-only 1d8ad71d66c06c726b3c5b35e367fec678031e85; Apple clang 21.0.0 clang-2100.3.27.1 arm64-apple-darwin27.0.0; -std=c11 -Os -ffunction-sections -fdata-sections -flto -Dyyjson_inline=inline -Wl,-dead_strip; system strip; TMPDIR in assigned worktree; quality completed, integration fixtures still running

| Metric | Baseline median ms | Candidate median ms | Delta ms | Result |
| --- | ---: | ---: | ---: | --- |
| help | 3.4033 | 3.3633 | -0.0400 | PASS |
| version | 3.2443 | 3.2410 | -0.0032 | PASS |
| prompt | 2.9762 | 2.9701 | -0.0061 | PASS |

## baseline

Stripped bytes: 1086288
C++ runtimes: none

```text
/Users/tomas/projects/tny-cpp-build/build/startup-baseline/tny:
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1359.0.0)
```

| Metric | p95 ms | Peak child RSS bytes |
| --- | ---: | ---: |
| help | 3.9463 | 2179072 |
| version | 3.5295 | 2179072 |
| prompt | 3.1587 | 2588672 |

Wasm accounting: `{"artifacts_bytes": {}, "available": false, "directory": "/Users/tomas/projects/tny-cpp-build/build/startup-baseline/wasm", "total_bytes": 0}`

## candidate

Stripped bytes: 1086288
C++ runtimes: none

```text
/Users/tomas/projects/tny-cpp-build/build/startup-baseline/tny:
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1359.0.0)
```

| Metric | p95 ms | Peak child RSS bytes |
| --- | ---: | ---: |
| help | 3.9595 | 2179072 |
| version | 3.8277 | 2179072 |
| prompt | 3.2088 | 2588672 |

Wasm accounting: `{"artifacts_bytes": {}, "available": false, "directory": "/Users/tomas/projects/tny-cpp-build/build/startup-baseline/wasm", "total_bytes": 0}`
