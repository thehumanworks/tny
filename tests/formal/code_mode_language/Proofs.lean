-- Definitions above this file are generated from the actual Python policy AST.
-- No hand-maintained copy of the implementation is used.
namespace CodeMode

theorem acceptance_exact (e o t : Bool) :
    accept e o t = true ↔ e = true ∧ o = true ∧ t = true := by
  simp [accept, Bool.and_eq_true, and_assoc]

theorem execution_required (o t : Bool) : accept false o t = false := by
  simp [accept]

theorem output_required (e t : Bool) : accept e false t = false := by
  simp [accept]

theorem trace_required (e o : Bool) : accept e o false = false := by
  simp [accept]

theorem accepted_execution (e o t : Bool) (h : accept e o t = true) : e = true := by
  exact (acceptance_exact e o t).mp h |>.1

theorem accepted_output (e o t : Bool) (h : accept e o t = true) : o = true := by
  exact (acceptance_exact e o t).mp h |>.2.1

theorem accepted_trace (e o t : Bool) (h : accept e o t = true) : t = true := by
  exact (acceptance_exact e o t).mp h |>.2.2

theorem acceptance_nonvacuous : accept true true true = true := by decide

theorem promotion_exact (c g p : Bool) (fc fb ec eb : Int) :
    promote c g p fc fb ec eb = true ↔
      c = true ∧ g = true ∧ p = true ∧ fc ≥ fb ∧ ec ≥ eb := by
  simp [promote, Bool.and_eq_true, and_assoc]

theorem no_incomplete_promotion (g p : Bool) (fc fb ec eb : Int) :
    promote false g p fc fb ec eb = false := by simp [promote]

theorem no_unconfirmed_promotion (c p : Bool) (fc fb ec eb : Int) :
    promote c false p fc fb ec eb = false := by simp [promote]

theorem no_unverified_parity (c g : Bool) (fc fb ec eb : Int) :
    promote c g false fc fb ec eb = false := by simp [promote]

theorem first_pass_nonregression (c g p : Bool) (fc fb ec eb : Int)
    (h : promote c g p fc fb ec eb = true) : fc ≥ fb := by
  exact (promotion_exact c g p fc fb ec eb).mp h |>.2.2.2.1

theorem eventual_nonregression (c g p : Bool) (fc fb ec eb : Int)
    (h : promote c g p fc fb ec eb = true) : ec ≥ eb := by
  exact (promotion_exact c g p fc fb ec eb).mp h |>.2.2.2.2

theorem incomplete_cannot_promote (c g p : Bool) (fc fb ec eb : Int)
    (h : promote c g p fc fb ec eb = true) : c = true := by
  exact (promotion_exact c g p fc fb ec eb).mp h |>.1

theorem no_first_pass_regression (c g p : Bool) (fc fb ec eb : Int) (h : fc < fb) :
    promote c g p fc fb ec eb = false := by
  simp [promote, Int.not_le.mpr h]

theorem no_eventual_regression (c g p : Bool) (fc fb ec eb : Int) (h : ec < eb) :
    promote c g p fc fb ec eb = false := by
  simp [promote, Int.not_le.mpr h]

theorem promotion_nonvacuous : promote true true true 36 36 36 36 = true := by decide

#print axioms acceptance_exact
#print axioms promotion_exact
end CodeMode
