/-
Independent properties of the preregistered runtime-selection gate.

check_runtime_selection.py prepends `PyPolicy.select_lighter`, generated from
the AST of tests/bench/python_runtime/policy.py (Python int -> Int, bool ->
Bool). These theorems say what "select the lighter runtime" requires; they
are about the decision rule, not about the stochastic trial that feeds it.
Argument order: corpus_passed corpus_total trials_complete first_lighter
first_cpython final_lighter final_cpython tokens_lighter solved_lighter
tokens_cpython solved_cpython semantics_ok bytes_lighter bytes_cpython.
-/

namespace RuntimeSelection
open PyPolicy

/-- Normalize the generated Boolean gate to propositions. -/
macro "tny_select" : tactic => `(tactic| (
  simp only [select_lighter, Bool.and_eq_true, decide_eq_true_eq, Bool.false_eq_true,
    and_false, false_and, and_true, true_and, Bool.and_false, Bool.false_and, Bool.and_true,
    Bool.true_and, iff_self]))

/-- The rule, stated independently: a fully passing non-empty corpus, complete
trials, no first-pass or final regression, positive solved counts, no more
output tokens per solved task (cross-multiplied, so no rounding), verified
production semantics, and a strictly smaller binary. -/
def Selected (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int) (sem : Bool)
    (bl bc : Int) : Prop :=
  0 < ct ∧ cp = ct ∧ tc = true ∧ fc ≤ fl ∧ ec ≤ el ∧ 0 < sl ∧ 0 < scp ∧
    tl * scp ≤ tcp * sl ∧ sem = true ∧ bl < bc

theorem select_iff (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int) (sem : Bool)
    (bl bc : Int) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = true ↔
      Selected cp ct tc fl fc el ec tl sl tcp scp sem bl bc := by
  cases tc <;> cases sem <;> simp only [Selected] <;> tny_select <;> omega

/-- Replace a `select_lighter … = true` hypothesis by the specification. -/
theorem selected_of (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int) (sem : Bool)
    (bl bc : Int) (h : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = true) :
    Selected cp ct tc fl fc el ec tl sl tcp scp sem bl bc :=
  (select_iff _ _ _ _ _ _ _ _ _ _ _ _ _ _).mp h

theorem corpus_must_fully_pass (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int)
    (sem : Bool) (bl bc : Int) (h : cp ≠ ct) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = false := by
  cases e : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc
  · rfl
  · exact absurd (selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e).2.1 h

theorem empty_corpus_rejected (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int)
    (sem : Bool) (bl bc : Int) (h : ct ≤ 0) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = false := by
  cases e : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc
  · rfl
  · have := (selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e).1; omega

theorem incomplete_trials_rejected (cp ct : Int) (fl fc el ec tl sl tcp scp : Int)
    (sem : Bool) (bl bc : Int) :
    select_lighter cp ct false fl fc el ec tl sl tcp scp sem bl bc = false := by
  cases e : select_lighter cp ct false fl fc el ec tl sl tcp scp sem bl bc
  · rfl
  · exact absurd (selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e).2.2.1 (by decide)

theorem no_first_pass_regression (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int)
    (sem : Bool) (bl bc : Int) (h : fl < fc) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = false := by
  cases e : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc
  · rfl
  · have := (selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e).2.2.2.1; omega

theorem no_final_regression (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int)
    (sem : Bool) (bl bc : Int) (h : el < ec) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = false := by
  cases e : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc
  · rfl
  · have := (selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e).2.2.2.2.1; omega

/-- Token efficiency is only compared when both arms solved something, so the
cross-multiplied criterion never stands in for a division by zero. -/
theorem unsolved_rejected (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int)
    (sem : Bool) (bl bc : Int) (h : sl ≤ 0 ∨ scp ≤ 0) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = false := by
  cases e : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc
  · rfl
  · have := selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e; simp only [Selected] at this; omega

theorem token_regression_rejected (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int)
    (sem : Bool) (bl bc : Int) (h : tcp * sl < tl * scp) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = false := by
  cases e : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc
  · rfl
  · have := selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e; simp only [Selected] at this; omega

/-- The token criterion compares ratios: scaling the lighter arm's tokens and
solved count by the same positive factor never changes the decision. -/
theorem token_criterion_scale_invariant (cp ct : Int) (tc : Bool)
    (fl fc el ec tl sl tcp scp : Int) (sem : Bool) (bl bc k : Int) (hk : 0 < k) :
    select_lighter cp ct tc fl fc el ec (k * tl) (k * sl) tcp scp sem bl bc =
      select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc := by
  have ratio : k * tl * scp ≤ tcp * (k * sl) ↔ tl * scp ≤ tcp * sl := by
    rw [Int.mul_assoc, Int.mul_left_comm tcp k sl]
    exact ⟨fun h => Int.le_of_mul_le_mul_left h hk,
      fun h => Int.mul_le_mul_of_nonneg_left h (Int.le_of_lt hk)⟩
  have positive : 0 < k * sl ↔ 0 < sl := by
    constructor
    · intro h
      exact Int.lt_of_mul_lt_mul_left (by rw [Int.mul_zero]; exact h) (Int.le_of_lt hk)
    · intro h; exact Int.mul_pos hk h
  apply Bool.eq_iff_iff.mpr
  rw [select_iff, select_iff]
  simp only [Selected, ratio, positive]

theorem semantics_required (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int)
    (bl bc : Int) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp false bl bc = false := by
  cases e : select_lighter cp ct tc fl fc el ec tl sl tcp scp false bl bc
  · rfl
  · exact absurd (selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e).2.2.2.2.2.2.2.2.1 (by decide)

/-- protocol.md: a lighter candidate that is not smaller than static CPython
cannot be selected, whatever the trial shows. -/
theorem size_gate (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int) (sem : Bool)
    (bl bc : Int) (h : bc ≤ bl) :
    select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = false := by
  cases e : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc
  · rfl
  · have := (selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ e).2.2.2.2.2.2.2.2.2; omega

theorem selected_is_smaller (cp ct : Int) (tc : Bool) (fl fc el ec tl sl tcp scp : Int)
    (sem : Bool) (bl bc : Int)
    (h : select_lighter cp ct tc fl fc el ec tl sl tcp scp sem bl bc = true) : bl < bc :=
  (selected_of _ _ _ _ _ _ _ _ _ _ _ _ _ _ h).2.2.2.2.2.2.2.2.2

/-- The gate can admit: a complete, non-regressing, cheaper, smaller runtime. -/
theorem nonvacuity :
    select_lighter 36 36 true 35 35 36 35 9000 36 8750 35 true 4000000 4668312 = true := by
  decide

end RuntimeSelection
