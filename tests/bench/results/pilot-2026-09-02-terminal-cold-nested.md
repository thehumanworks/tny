## Tool-profile A/B — 2026-09-02

provider `aiproxy`, model `grok-4.6`, effort `high`, N=1 per task per arm, --max-steps 40, timeout 600s, tny `0.3.3-24-g261552f`

| Arm | Pass | Steps | Tool calls | Tok in | Tok out | Wall s | Repairs | tny edit | edit_file | shell write |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `terminal` | 26/26 (100%) | 4.6 | 3.6 | 1948 | 79 | 10.3 | 14 | 11 | 0 | 14 |

### Per-task pass matrix

| Task | Family | `terminal` |
| --- | --- | --- |
| fix-c-clamp | fix-test | pass |
| fix-c-count-words | fix-test | pass |
| fix-c-gcd | fix-test | pass |
| fix-c-strrev | fix-test | pass |
| fix-py-chunk | fix-test | pass |
| fix-py-fizzbuzz | fix-test | pass |
| fix-py-median | fix-test | pass |
| fix-py-parse-kv | fix-test | pass |
| fix-py-rle | fix-test | pass |
| fix-py-sum-range | fix-test | pass |
| q-call-flush-all | question | pass |
| q-const-max-retries | question | pass |
| q-decl-list-len | question | pass |
| q-def-compute-total | question | pass |
| q-def-list-sum | question | pass |
| q-import-hashlib | question | pass |
| q-struct-node | question | pass |
| q-todo-line | question | pass |
| rename-c-buf | refactor | pass |
| rename-c-hash | refactor | pass |
| rename-c-log | refactor | pass |
| rename-c-vec | refactor | pass |
| rename-py-cfg | refactor | pass |
| rename-py-normalize | refactor | pass |
| rename-py-retry | refactor | pass |
| rename-py-token | refactor | pass |

### By family

| Family | `terminal` |
| --- | ---: |
| fix-test | 10/10 |
| question | 8/8 |
| refactor | 8/8 |

