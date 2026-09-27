/-
Proofs about the code-cell gates generated from src/core/code_policy.c by
tests/formal/check_code_policy.py (definitions precede this file in the same
Lean input). Section A is about the translated production definitions.
Section B is an ABSTRACT model of the parent loop in src/core/code_runtime.c
that composes the translated gate; it is not itself translated from C.
-/
namespace TnyCodePolicy

def W : Int := 18446744073709551616
def U64 (x : Int) : Prop := 0 ≤ x ∧ x < W

/-! ## A. Source-linked gates -/

theorem timeout_exact (t : Int) :
    tny_code_timeout_admit t = true ↔ 1 ≤ t ∧ t ≤ 30000 := by
  simp [tny_code_timeout_admit]

theorem timeout_nonvacuous :
    tny_code_timeout_admit 1 = true ∧ tny_code_timeout_admit 30000 = true ∧
    tny_code_timeout_admit 0 = false ∧ tny_code_timeout_admit 30001 = false := by
  decide

theorem source_exact (b : Int) : tny_code_source_admit b = true ↔ b ≤ 262144 := by
  simp [tny_code_source_admit]

theorem call_exact (c n : Int) (r : Bool) (a : Int) (o : Bool) :
    tny_code_call_admit c n r a o = true ↔
      c < 64 ∧ 1 ≤ n ∧ n ≤ 256 ∧ r = false ∧ a ≤ 262144 ∧ o = true := by
  cases r <;> cases o <;> simp [tny_code_call_admit] <;> omega

/-- The 65th nested call is never admitted, whatever else holds. -/
theorem call_budget (c n : Int) (r : Bool) (a : Int) (o : Bool) (h : 64 ≤ c) :
    tny_code_call_admit c n r a o = false := by
  cases r <;> cases o <;> simp [tny_code_call_admit] <;> omega

theorem call_recursion_refused (c n a : Int) (o : Bool) :
    tny_code_call_admit c n true a o = false := by
  cases o <;> simp [tny_code_call_admit]

theorem call_nonvacuous :
    tny_code_call_admit 63 1 false 262144 true = true ∧
    tny_code_call_admit 0 256 false 2 true = true ∧
    tny_code_call_admit 0 257 false 2 true = false ∧
    tny_code_call_admit 0 4 false 2 false = false := by
  decide

theorem result_exact (l : Int) : tny_code_result_admit l = true ↔ l ≤ 8388592 := by
  simp [tny_code_result_admit]

/-- No wrap-around: an admitted append keeps the total within 64 KiB. -/
theorem output_sound (u d : Int) (hu : U64 u)
    (h : tny_code_output_admit u d = true) : u + d ≤ 65536 := by
  simp [tny_code_output_admit, U64, W] at *
  omega

theorem output_complete (u d : Int) (hu : 0 ≤ u) (hd : 0 ≤ d) (h : u + d ≤ 65536) :
    tny_code_output_admit u d = true := by
  simp [tny_code_output_admit]
  omega

theorem output_nonvacuous :
    tny_code_output_admit 0 65536 = true ∧ tny_code_output_admit 1 65536 = false ∧
    tny_code_output_admit 65537 0 = false := by
  decide

/-- Heap accounting never overflows: admitted means the charge fits the limit. -/
theorem memory_sound (u r h l : Int) (hu : U64 u) (hr : U64 r) (hh : U64 h) (hl : U64 l)
    (ok : tny_code_memory_admit u r h l = true) : u + h + r ≤ l := by
  simp [tny_code_memory_admit, U64, W] at *
  omega

theorem memory_complete (u r h l : Int) (hr : 0 ≤ r) (hh : 0 ≤ h) (hu : 0 ≤ u)
    (fits : u + h + r ≤ l) (hl : l < W) : tny_code_memory_admit u r h l = true := by
  simp [tny_code_memory_admit, U64, W] at *
  omega

theorem memory_nonvacuous :
    tny_code_memory_admit 0 10 16 26 = true ∧ tny_code_memory_admit 0 11 16 26 = false ∧
    tny_code_memory_admit 18446744073709551614 1 16 18446744073709551615 = false := by
  decide

/-- Only a RUNNING parent admits frames, only CALL or DONE, CALL within budget. -/
theorem frame_running_only (p t l c : Int) (h : tny_code_frame_admit p t l c = true) :
    p = 1 := by
  simp [tny_code_frame_admit] at h
  omega

theorem frame_types (p t l c : Int) (h : tny_code_frame_admit p t l c = true) :
    t = 67 ∨ t = 68 := by
  simp [tny_code_frame_admit] at h
  omega

theorem frame_call_budget (p l c : Int) (h : tny_code_frame_admit p 67 l c = true) :
    c < 64 := by
  simp [tny_code_frame_admit] at h
  omega

/-- The parent's terminal path sets the spent budget: no call frame after it. -/
theorem frame_spent_budget_refuses_calls (l : Int) : tny_code_frame_admit 1 67 l 64 = false := by
  simp [tny_code_frame_admit]

theorem frame_done_bounded (p l c : Int) (h : tny_code_frame_admit p 68 l c = true) :
    1 ≤ l ∧ l ≤ 66049 := by
  simp [tny_code_frame_admit] at h
  omega

theorem frame_finished_refuses (t l c : Int) : tny_code_frame_admit 2 t l c = false := by
  simp [tny_code_frame_admit]

theorem frame_failed_refuses (t l c : Int) : tny_code_frame_admit 3 t l c = false := by
  simp [tny_code_frame_admit]

theorem frame_nonvacuous :
    tny_code_frame_admit 1 67 3 0 = true ∧ tny_code_frame_admit 1 68 1 64 = true ∧
    tny_code_frame_admit 1 82 2 0 = false ∧ tny_code_frame_admit 1 68 66050 0 = false := by
  decide

/-- JSON codec: None first, then bool before int, so false never encodes as 0. -/
theorem json_none_first (b i f s a d : Bool) :
    tny_code_json_kind true b i f s a d = 0 := by
  simp [tny_code_json_kind]

theorem json_bool_before_int (i f s a d : Bool) :
    tny_code_json_kind false true i f s a d = 1 := by
  simp [tny_code_json_kind]

theorem json_int_only_when_not_bool (f s a d : Bool) :
    tny_code_json_kind false false true f s a d = 2 := by
  simp [tny_code_json_kind]

theorem json_kinds_nonvacuous :
    tny_code_json_kind false false false true false false false = 3 ∧
    tny_code_json_kind false false false false true false false = 4 ∧
    tny_code_json_kind false false false false false true false = 5 ∧
    tny_code_json_kind false false false false false false true = 6 ∧
    tny_code_json_kind false false false false false false false = 7 := by
  decide

/-! ## B. Abstract parent-loop model (composes the translated frame gate) -/

inductive Frame where
  | call (len : Int)
  | done (len : Int)
  | other (ty len : Int)

def Frame.ty : Frame → Int
  | .call _ => 67
  | .done _ => 68
  | .other t _ => t

def Frame.len : Frame → Int
  | .call l => l
  | .done l => l
  | .other _ l => l

structure Parent where
  phase : Int
  calls : Int
  executed : Int

/-- One child frame. `fails` models a NULL callback or oversized result: the
parent then sets the spent budget (calls := 64), as code_runtime.c does. -/
def step (s : Parent) (f : Frame) (fails : Bool) : Parent :=
  if f.ty ≠ 67 ∧ f.ty ≠ 68 then { s with phase := 3 }
  else if tny_code_frame_admit s.phase f.ty f.len s.calls then
    if f.ty = 67 then
      if fails then { s with calls := 64, executed := s.executed + 1 }
      else { s with calls := s.calls + 1, executed := s.executed + 1 }
    else { s with phase := 2 }
  else { s with phase := 3 }

def run (s : Parent) : List (Frame × Bool) → Parent
  | [] => s
  | (f, fails) :: rest => run (step s f fails) rest

theorem step_invariant (s : Parent) (f : Frame) (fails : Bool)
    (h : 0 ≤ s.executed ∧ s.executed ≤ s.calls ∧ s.calls ≤ 64) :
    let t := step s f fails
    0 ≤ t.executed ∧ t.executed ≤ t.calls ∧ t.calls ≤ 64 := by
  unfold step
  split
  · simp_all
  · split
    · rename_i hadmit
      have hc : f.ty = 67 → s.calls < 64 := by
        intro h67
        rw [h67] at hadmit
        exact frame_call_budget _ _ _ hadmit
      split
      · rename_i h67
        have := hc h67
        split <;> simp <;> omega
      · simp_all
    · simp_all

/-- Whatever frames an untrusted child sends and whichever callbacks fail, the
parent executes at most 64 nested calls. -/
theorem executed_bounded (frames : List (Frame × Bool)) :
    ∀ s : Parent, 0 ≤ s.executed ∧ s.executed ≤ s.calls ∧ s.calls ≤ 64 →
      (run s frames).executed ≤ 64 := by
  induction frames with
  | nil => intro s h; simp [run]; omega
  | cons head rest ih =>
    intro s h
    simp only [run]
    exact ih _ (step_invariant s head.1 head.2 h)

theorem fresh_cell_bounded (frames : List (Frame × Bool)) :
    (run { phase := 1, calls := 0, executed := 0 } frames).executed ≤ 64 :=
  executed_bounded frames _ (by simp)

-- Axiom audit (printed by the checker; no sorryAx may appear).
#print axioms call_budget
#print axioms output_sound
#print axioms memory_sound
#print axioms frame_spent_budget_refuses_calls
#print axioms json_bool_before_int
#print axioms fresh_cell_bounded

end TnyCodePolicy
