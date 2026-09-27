/-
Independent specification and proofs for the Python code-cell gates.

tests/formal/check_code_policy.py prepends the `TnyC` definitions it generates
from the Clang AST of src/core/code_policy.c (exact C widths: `BitVec 64` for
uint64_t/int64_t, `BitVec 32` for int, `Bool` for _Bool) and appends generated
no-wrap obligations, so this file never contains a copy of the C code. The
limits below are the intended contract (code_policy.h / code_runtime.h, ADR
0179) restated as plain numbers; changing a production limit fails these
theorems until the contract is deliberately updated here.
-/

/-- Normalize generated BitVec gates to natural-number arithmetic. -/
macro "tny_bv" : tactic => `(tactic| (
  simp only [Bool.and_eq_true, Bool.or_eq_true, Bool.not_eq_true', Bool.not_eq_false',
    Bool.and_true, Bool.true_and, Bool.and_false, Bool.false_and, Bool.not_true,
    Bool.not_false, Bool.or_false, Bool.false_or, and_true, true_and,
    BitVec.ule, BitVec.ult, decide_eq_true_eq, decide_eq_false_iff_not, BitVec.toNat_eq,
    BitVec.toNat_sub, BitVec.toNat_add, BitVec.toNat_mul, BitVec.toNat_ofNat,
    BitVec.reduceMul, BitVec.reduceAdd, BitVec.reduceSub, BitVec.reduceZeroExtend,
    BitVec.reduceSignExtend, Bool.false_eq_true, Bool.true_eq_false, and_false, false_and,
    iff_false, false_iff, not_and, not_false_eq_true, imp_false]
    at *))

/-- Discharge a generated no-wrap obligation. -/
macro "tny_no_wrap" : tactic => `(tactic| (intros; tny_bv; omega))

namespace CodePolicy
open TnyC

/-! ## Contract, restated independently of the C source -/

def MaxTimeoutMs : Int := 30000
def SourceBytes : Nat := 256 * 1024
def ToolCalls : Nat := 64
def NameBytes : Nat := 256
def ArgumentBytes : Nat := 256 * 1024
def FrameBytes : Nat := 8 * 1024 * 1024
def FrameHeaderBytes : Nat := 16
def OutputBytes : Nat := 64 * 1024
def ErrorLineBytes : Nat := 512
/-- Parent phases and child frame types (first payload byte). -/
def Running : Int := 1
def Finished : Int := 2
def Failed : Int := 3
def FrameCall : Int := 67 -- 'C'
def FrameDone : Int := 68 -- 'D'
/-- JSON kinds, in the facade's classification order. -/
def JNull : Nat := 0
def JBool : Nat := 1
def JInt : Nat := 2
def JFloat : Nat := 3
def JString : Nat := 4
def JArray : Nat := 5
def JObject : Nat := 6
def JDefault : Nat := 7

/-- A CALL frame is 'C', the name, '\n' and the JSON arguments; DONE is 'D' and
the final text: bounded output plus at most one error line. -/
def FrameSpec (phase type : Int) (payload calls : Nat) : Prop :=
  phase = Running ∧ 1 ≤ payload ∧
    ((type = FrameCall ∧ calls < ToolCalls ∧ payload ≤ 1 + NameBytes + 1 + ArgumentBytes) ∨
     (type = FrameDone ∧ payload ≤ 1 + OutputBytes + ErrorLineBytes))

/-- First matching Python type test wins: None, then bool (an int subclass),
then int, float, str, list/tuple, dict; anything else uses the default hook. -/
def JsonSpec (none bool int float str list dict : Bool) : Nat :=
  if none then JNull else if bool then JBool else if int then JInt else
  if float then JFloat else if str then JString else if list then JArray else
  if dict then JObject else JDefault

/-- C `int` values that fit are the same as their bit patterns. -/
theorem toInt32_eq_small (x : BitVec 32) (k : Nat) (hk : k < 2 ^ 31) :
    x.toInt = k ↔ x.toNat = k := by
  unfold BitVec.toInt
  have := x.isLt
  split <;> omega

/-! ## Timeout and source -/

theorem timeout_admit_iff (t : BitVec 64) :
    tny_code_timeout_admit t = true ↔ 1 ≤ t.toInt ∧ t.toInt ≤ MaxTimeoutMs := by
  simp [tny_code_timeout_admit, BitVec.sle, MaxTimeoutMs]

theorem timeout_boundaries :
    tny_code_timeout_admit 0#64 = false ∧ tny_code_timeout_admit 1#64 = true ∧
    tny_code_timeout_admit 30000#64 = true ∧ tny_code_timeout_admit 30001#64 = false ∧
    tny_code_timeout_admit (BitVec.ofInt 64 (-1)) = false ∧
    tny_code_timeout_admit (BitVec.ofInt 64 (-30000)) = false ∧
    tny_code_timeout_admit (BitVec.ofInt 64 (-2 ^ 63)) = false ∧
    tny_code_timeout_admit (BitVec.ofInt 64 (2 ^ 63 - 1)) = false ∧
    ∀ t : BitVec 64, t.toInt ≤ 0 → tny_code_timeout_admit t = false := by
  refine ⟨by decide, by decide, by decide, by decide, by decide, by decide, by decide,
    by decide, fun t h => ?_⟩
  cases e : tny_code_timeout_admit t
  · rfl
  · have := (timeout_admit_iff t).mp e; omega

theorem source_admit_iff (b : BitVec 64) :
    tny_code_source_admit b = true ↔ b.toNat ≤ SourceBytes := by
  simp only [tny_code_source_admit, SourceBytes]; tny_bv

theorem source_boundaries :
    tny_code_source_admit 0#64 = true ∧ tny_code_source_admit 262144#64 = true ∧
    tny_code_source_admit 262145#64 = false ∧ tny_code_source_admit (-1#64) = false := by
  decide

/-! ## Nested calls: at most 64, bounded name and object arguments -/

theorem call_admit_iff (c n a : BitVec 64) (r o : Bool) :
    tny_code_call_admit c n r a o = true ↔
      c.toNat < ToolCalls ∧ 1 ≤ n.toNat ∧ n.toNat ≤ NameBytes ∧ r = false ∧
        a.toNat ≤ ArgumentBytes ∧ o = true := by
  cases r <;> cases o <;> simp only [tny_code_call_admit, ToolCalls, NameBytes, ArgumentBytes] <;>
    tny_bv <;> omega

theorem call_boundaries :
    tny_code_call_admit 63#64 1#64 false 262144#64 true = true ∧
    tny_code_call_admit 64#64 1#64 false 0#64 true = false ∧
    tny_code_call_admit 0#64 0#64 false 0#64 true = false ∧
    tny_code_call_admit 0#64 256#64 false 0#64 true = true ∧
    tny_code_call_admit 0#64 257#64 false 0#64 true = false ∧
    tny_code_call_admit 0#64 1#64 true 0#64 true = false ∧
    tny_code_call_admit 0#64 1#64 false 262145#64 true = false ∧
    tny_code_call_admit 0#64 1#64 false 0#64 false = false := by
  decide

/-- No 65th call: once 64 calls are done, nothing is admitted. -/
theorem no_call_after_budget (c n a : BitVec 64) (r o : Bool) (h : ToolCalls ≤ c.toNat) :
    tny_code_call_admit c n r a o = false := by
  cases e : tny_code_call_admit c n r a o
  · rfl
  · have := (call_admit_iff c n a r o).mp e; omega

/-- ... and every one of the first 64 calls can be admitted. -/
theorem every_budgeted_call_admitted (c : BitVec 64) (h : c.toNat < ToolCalls) :
    tny_code_call_admit c 1#64 false 2#64 true = true := by
  rw [call_admit_iff]; simp [ToolCalls, NameBytes, ArgumentBytes] at *; omega

/-! ## Tool results -/

theorem result_admit_iff (r : BitVec 64) :
    tny_code_result_admit r = true ↔ r.toNat ≤ FrameBytes - FrameHeaderBytes := by
  simp only [tny_code_result_admit, FrameBytes, FrameHeaderBytes]; tny_bv

theorem result_boundaries :
    tny_code_result_admit 8388592#64 = true ∧ tny_code_result_admit 8388593#64 = false ∧
    tny_code_result_admit (-1#64) = false := by
  decide

/-- An admitted nested result plus the frame header fits one 8 MiB frame. -/
theorem result_fits_frame (r : BitVec 64) (h : tny_code_result_admit r = true) :
    r.toNat + FrameHeaderBytes ≤ FrameBytes := by
  have := (result_admit_iff r).mp h; simp [FrameBytes, FrameHeaderBytes] at *; omega

/-! ## Printed output: guarded subtraction, no overflow -/

theorem output_admit_iff (u a : BitVec 64) :
    tny_code_output_admit u a = true ↔ u.toNat + a.toNat ≤ OutputBytes := by
  simp only [tny_code_output_admit, OutputBytes]; tny_bv; omega

/-- The admitted C sum `used + add` is exact (no uint64_t wrap) and in bounds. -/
theorem output_admitted_sum_exact (u a : BitVec 64) (h : tny_code_output_admit u a = true) :
    (u + a).toNat = u.toNat + a.toNat ∧ (u + a).toNat ≤ OutputBytes := by
  have := (output_admit_iff u a).mp h
  simp only [OutputBytes, BitVec.toNat_add] at *; omega

theorem output_boundaries :
    tny_code_output_admit 0#64 65536#64 = true ∧ tny_code_output_admit 0#64 65537#64 = false ∧
    tny_code_output_admit 65536#64 0#64 = true ∧ tny_code_output_admit 65536#64 1#64 = false ∧
    tny_code_output_admit 65537#64 0#64 = false ∧ tny_code_output_admit 1#64 (-1#64) = false ∧
    tny_code_output_admit (-1#64) 1#64 = false := by
  decide

/-- Sums that would wrap modulo 2^64 are rejected, whatever the operands. -/
theorem output_wrap_rejected (u a : BitVec 64) (h : 2 ^ 64 ≤ u.toNat + a.toNat) :
    tny_code_output_admit u a = false := by
  cases e : tny_code_output_admit u a
  · rfl
  · have := (output_admit_iff u a).mp e; simp [OutputBytes] at this; omega

/-! ## Interpreter heap accounting -/

theorem memory_admit_iff (u r h l : BitVec 64) :
    tny_code_memory_admit u r h l = true ↔ u.toNat + h.toNat + r.toNat ≤ l.toNat := by
  simp only [tny_code_memory_admit]; tny_bv; omega

theorem memory_admitted_sum_exact (u r h l : BitVec 64)
    (e : tny_code_memory_admit u r h l = true) :
    (u + h + r).toNat = u.toNat + h.toNat + r.toNat ∧ (u + h + r).toNat ≤ l.toNat := by
  have := (memory_admit_iff u r h l).mp e
  simp only [BitVec.toNat_add] at *; omega

theorem memory_boundaries :
    let l := 67108864#64
    tny_code_memory_admit 0#64 67108848#64 16#64 l = true ∧
    tny_code_memory_admit 0#64 67108849#64 16#64 l = false ∧
    tny_code_memory_admit l 0#64 0#64 l = true ∧
    tny_code_memory_admit 67108865#64 0#64 0#64 l = false ∧
    tny_code_memory_admit 1#64 (-1#64) 0#64 l = false ∧
    tny_code_memory_admit 1#64 0#64 (-1#64) l = false ∧
    tny_code_memory_admit 16#64 (-1#64) 17#64 l = false := by
  decide

/-! ## Parent admission of child frames -/

theorem frame_admit_iff (p t : BitVec 32) (n c : BitVec 64) :
    tny_code_frame_admit p t n c = true ↔ FrameSpec p.toInt t.toInt n.toNat c.toNat := by
  simp only [FrameSpec, Running, FrameCall, FrameDone, ToolCalls, NameBytes, ArgumentBytes,
    OutputBytes, ErrorLineBytes]
  rw [show (1 : Int) = ((1 : Nat) : Int) from rfl, toInt32_eq_small p 1 (by decide),
    show (67 : Int) = ((67 : Nat) : Int) from rfl, toInt32_eq_small t 67 (by decide),
    show (68 : Int) = ((68 : Nat) : Int) from rfl, toInt32_eq_small t 68 (by decide)]
  simp only [tny_code_frame_admit]; tny_bv; omega

theorem frame_requires_running (p t : BitVec 32) (n c : BitVec 64)
    (h : tny_code_frame_admit p t n c = true) : p.toInt = Running :=
  ((frame_admit_iff p t n c).mp h).1

/-- FINISHED (DONE seen) and FAILED admit nothing at all. -/
theorem no_terminal_phase_frames (t : BitVec 32) (n c : BitVec 64) :
    tny_code_frame_admit (BitVec.ofInt 32 Finished) t n c = false ∧
    tny_code_frame_admit (BitVec.ofInt 32 Failed) t n c = false := by
  constructor <;>
  · cases e : tny_code_frame_admit _ t n c
    · rfl
    · have := frame_requires_running _ t n c e; revert this; decide

theorem frame_only_call_or_done (p t : BitVec 32) (n c : BitVec 64)
    (h : tny_code_frame_admit p t n c = true) : t.toInt = FrameCall ∨ t.toInt = FrameDone := by
  have := (frame_admit_iff p t n c).mp h; simp only [FrameSpec] at this; omega

theorem frame_nonempty (p t : BitVec 32) (c : BitVec 64) :
    tny_code_frame_admit p t 0#64 c = false := by
  cases e : tny_code_frame_admit p t 0#64 c
  · rfl
  · have := (frame_admit_iff p t _ c).mp e; simp [FrameSpec] at this

/-- The parent refuses a 65th CALL frame even if the child's gate were bypassed. -/
theorem no_call_frame_after_budget (p : BitVec 32) (n c : BitVec 64) (h : ToolCalls ≤ c.toNat) :
    tny_code_frame_admit p (BitVec.ofInt 32 FrameCall) n c = false := by
  cases e : tny_code_frame_admit p _ n c
  · rfl
  · have := (frame_admit_iff p _ n c).mp e
    simp only [FrameSpec, FrameCall, FrameDone] at this
    have : (BitVec.ofInt 32 67).toInt = 67 := by decide
    omega

theorem frame_boundaries :
    let run := 1#32; let call := 67#32; let done := 68#32
    tny_code_frame_admit run call 262402#64 63#64 = true ∧
    tny_code_frame_admit run call 262403#64 0#64 = false ∧
    tny_code_frame_admit run call 1#64 64#64 = false ∧
    tny_code_frame_admit run done 66049#64 64#64 = true ∧
    tny_code_frame_admit run done 66050#64 0#64 = false ∧
    tny_code_frame_admit run done 0#64 0#64 = false ∧
    tny_code_frame_admit 0#32 done 1#64 0#64 = false ∧
    tny_code_frame_admit (BitVec.ofInt 32 (-1)) done 1#64 0#64 = false ∧
    tny_code_frame_admit run 83#32 1#64 0#64 = false ∧
    tny_code_frame_admit run 82#32 1#64 0#64 = false ∧
    tny_code_frame_admit run 70#32 1#64 0#64 = false := by
  decide

/-- Parent and child gates agree: every call the child admits yields a CALL
frame ('C' + name + '\n' + arguments) that the parent admits. -/
theorem admitted_call_fits_call_frame (c n a p : BitVec 64) (r o : Bool)
    (h : tny_code_call_admit c n r a o = true) (hp : p.toNat = 1 + n.toNat + 1 + a.toNat) :
    tny_code_frame_admit (1#32) (67#32) p c = true := by
  have := (call_admit_iff c n a r o).mp h
  rw [frame_admit_iff]
  simp only [FrameSpec, Running, FrameCall, ToolCalls, NameBytes, ArgumentBytes] at *
  refine ⟨by decide, by omega, Or.inl ⟨by decide, by omega, by omega⟩⟩

/-- Any admitted output plus one error line of at most 512 bytes fits DONE. -/
theorem admitted_output_fits_done_frame (u a e p c : BitVec 64)
    (h : tny_code_output_admit u a = true) (he : e.toNat ≤ ErrorLineBytes)
    (hp : p.toNat = 1 + u.toNat + a.toNat + e.toNat) :
    tny_code_frame_admit (1#32) (68#32) p c = true := by
  have := (output_admit_iff u a).mp h
  rw [frame_admit_iff]
  simp only [FrameSpec, Running, FrameDone, OutputBytes, ErrorLineBytes] at *
  refine ⟨by decide, by omega, Or.inr ⟨by decide, by omega⟩⟩

/-! ## Python value -> JSON kind -/

theorem json_kind_first_match : ∀ a b c d e f g : Bool,
    (tny_code_json_kind a b c d e f g).toNat = JsonSpec a b c d e f g := by
  decide

theorem json_kind_in_range : ∀ a b c d e f g : Bool,
    (tny_code_json_kind a b c d e f g).toNat ≤ JDefault := by
  decide

/-- None is checked first: whatever other tests claim, None is null. -/
theorem json_null_first : ∀ b c d e f g : Bool,
    (tny_code_json_kind true b c d e f g).toNat = JNull := by
  decide

/-- No null confusion: only None encodes as null. -/
theorem json_null_exact : ∀ a b c d e f g : Bool,
    (tny_code_json_kind a b c d e f g).toNat = JNull ↔ a = true := by
  decide

/-- bool is tested before int (bool is an int subclass): True/False stay booleans. -/
theorem json_bool_before_int : ∀ c d e f g : Bool,
    (tny_code_json_kind false true c d e f g).toNat = JBool := by
  decide

/-- A bool is never encoded as a number: false never becomes 0. -/
theorem json_bool_never_int : ∀ a c d e f g : Bool,
    (tny_code_json_kind a true c d e f g).toNat ≠ JInt := by
  decide

theorem json_int_exact : ∀ a b c d e f g : Bool,
    (tny_code_json_kind a b c d e f g).toNat = JInt ↔ a = false ∧ b = false ∧ c = true := by
  decide

theorem json_default_exact : ∀ a b c d e f g : Bool,
    (tny_code_json_kind a b c d e f g).toNat = JDefault ↔
      (a || b || c || d || e || f || g) = false := by
  decide

/-! ## Non-vacuity: every gate admits something and every kind is reachable -/

theorem nonvacuity :
    tny_code_timeout_admit 5000#64 = true ∧ tny_code_source_admit 1#64 = true ∧
    tny_code_call_admit 0#64 4#64 false 2#64 true = true ∧
    tny_code_result_admit 1#64 = true ∧ tny_code_output_admit 10#64 10#64 = true ∧
    tny_code_memory_admit 1024#64 64#64 16#64 67108864#64 = true ∧
    tny_code_frame_admit 1#32 67#32 8#64 0#64 = true ∧
    tny_code_frame_admit 1#32 68#32 1#64 0#64 = true ∧
    (tny_code_json_kind false false false true false false false).toNat = JFloat ∧
    (tny_code_json_kind false false false false true false false).toNat = JString ∧
    (tny_code_json_kind false false false false false true false).toNat = JArray ∧
    (tny_code_json_kind false false false false false false true).toNat = JObject := by
  decide

end CodePolicy
