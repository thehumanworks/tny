"""Small deterministic gates translated from this Python AST into Lean."""


def accept(execution_ok: bool, output_ok: bool, trace_ok: bool) -> bool:
    return execution_ok and output_ok and trace_ok


def promote(
    complete: bool,
    confirmed_gain: bool,
    parity: bool,
    first_candidate: int,
    first_baseline: int,
    final_candidate: int,
    final_baseline: int,
) -> bool:
    return (
        complete
        and confirmed_gain
        and parity
        and first_candidate >= first_baseline
        and final_candidate >= final_baseline
    )
