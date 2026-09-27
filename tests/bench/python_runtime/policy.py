"""Runtime-selection gate translated from this Python AST into Lean.

Committed before the held-out Luna trials. Integers only: output tokens per
solved task are compared by cross-multiplication, so no rounding is involved.
"""


def select_lighter(
    corpus_passed: int,
    corpus_total: int,
    trials_complete: bool,
    first_lighter: int,
    first_cpython: int,
    final_lighter: int,
    final_cpython: int,
    tokens_lighter: int,
    solved_lighter: int,
    tokens_cpython: int,
    solved_cpython: int,
    semantics_ok: bool,
    bytes_lighter: int,
    bytes_cpython: int,
) -> bool:
    return (
        corpus_total > 0
        and corpus_passed == corpus_total
        and trials_complete
        and first_lighter >= first_cpython
        and final_lighter >= final_cpython
        and solved_lighter > 0
        and solved_cpython > 0
        and tokens_lighter * solved_cpython <= tokens_cpython * solved_lighter
        and semantics_ok
        and bytes_lighter < bytes_cpython
    )
