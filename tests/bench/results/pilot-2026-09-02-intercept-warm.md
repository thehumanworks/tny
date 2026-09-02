## Tool-profile A/B — 2026-09-02

provider `aiproxy`, model `grok-4.6`, effort `high`, N=1 per task per arm, --max-steps 40, timeout 600s, tny `0.3.3-30-gac0bd11`

| Arm | Pass | Steps | Tool calls | Tok in | Tok out | Wall s | Repairs | tny edit | edit_file | shell write |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `all` | 26/26 (100%) | 4.3 | 5.7 | 3052 | 80 | 12.0 | 0 | 0 | 42 | 0 |
| `terminal+edit` | 26/26 (100%) | 4.1 | 4.0 | 1942 | 84 | 14.5 | 5 | 0 | 44 | 8 |
| `terminal` | 26/26 (100%) | 4.4 | 3.5 | 1864 | 75 | 10.7 | 10 | 18 | 0 | 12 |

### Per-task pass matrix

| Task | Family | `all` | `terminal+edit` | `terminal` |
| --- | --- | --- | --- | --- |
| fix-c-clamp | fix-test | pass | pass | pass |
| fix-c-count-words | fix-test | pass | pass | pass |
| fix-c-gcd | fix-test | pass | pass | pass |
| fix-c-strrev | fix-test | pass | pass | pass |
| fix-py-chunk | fix-test | pass | pass | pass |
| fix-py-fizzbuzz | fix-test | pass | pass | pass |
| fix-py-median | fix-test | pass | pass | pass |
| fix-py-parse-kv | fix-test | pass | pass | pass |
| fix-py-rle | fix-test | pass | pass | pass |
| fix-py-sum-range | fix-test | pass | pass | pass |
| q-call-flush-all | question | pass | pass | pass |
| q-const-max-retries | question | pass | pass | pass |
| q-decl-list-len | question | pass | pass | pass |
| q-def-compute-total | question | pass | pass | pass |
| q-def-list-sum | question | pass | pass | pass |
| q-import-hashlib | question | pass | pass | pass |
| q-struct-node | question | pass | pass | pass |
| q-todo-line | question | pass | pass | pass |
| rename-c-buf | refactor | pass | pass | pass |
| rename-c-hash | refactor | pass | pass | pass |
| rename-c-log | refactor | pass | pass | pass |
| rename-c-vec | refactor | pass | pass | pass |
| rename-py-cfg | refactor | pass | pass | pass |
| rename-py-normalize | refactor | pass | pass | pass |
| rename-py-retry | refactor | pass | pass | pass |
| rename-py-token | refactor | pass | pass | pass |

### By family

| Family | `all` | `terminal+edit` | `terminal` |
| --- | ---: | ---: | ---: |
| fix-test | 10/10 | 10/10 | 10/10 |
| question | 8/8 | 8/8 | 8/8 |
| refactor | 8/8 | 8/8 | 8/8 |

