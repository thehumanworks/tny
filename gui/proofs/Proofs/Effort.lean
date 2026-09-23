/-!
# Reasoning-effort picker (gui/src/main.rs `Picker`)

The composer's effort picker sits next to the provider/model picker. What it
offers depends on the selected provider and model: a model's catalog row
(`tny --provider P models --json`) may advertise `efforts`; otherwise the GUI
offers the CLI's generic levels (`off light medium high xhigh max`). "Default"
(`none`) passes no `--effort` and leaves the CLI's settings/provider default.

The selection, the catalogs and the chosen effort change independently
(a model pick, a catalog arriving later, opening a saved chat). After every one
of those the GUI re-clamps the effort (`Picker::clamp_effort`). The theorems:

* `inv_step` — the chosen effort is always one the current selection offers,
  so the picker never shows (or sends) an effort the model does not support;
* `pick_unoffered_is_ignored` — a pick outside the offer changes nothing;
* `clamp_keeps_supported` — re-clamping never discards a still-supported choice;
* `effort_args_valid` — `--effort X` is emitted iff an effort is chosen, and X is
  always a value some validated list offered (so never flag-like).

`Sel` and `E` are abstract (a provider/model pair and an effort token), so the
results hold for every catalog the CLI can return.
-/

namespace Tny.Effort

variable {Sel E : Type} [DecidableEq E]

structure St (Sel E : Type) where
  sel : Sel
  effort : Option E
  /-- Advertised efforts per selection; `none` when unknown. -/
  cat : Sel → Option (List E)

def allowed (generic : List E) (s : St Sel E) : List E := (s.cat s.sel).getD generic

def clamp (al : List E) : Option E → Option E
  | none => none
  | some e => if e ∈ al then some e else none

inductive Ev (Sel E : Type) where
  | pickEffort (e : Option E)
  | select (x : Sel)
  | catalog (c : Sel → Option (List E))

def reclamp (generic : List E) (s : St Sel E) : St Sel E :=
  { s with effort := clamp (allowed generic s) s.effort }

def step (generic : List E) (s : St Sel E) : Ev Sel E → St Sel E
  | .pickEffort none => { s with effort := none }
  | .pickEffort (some e) => if e ∈ allowed generic s then { s with effort := some e } else s
  | .select x => reclamp generic { s with sel := x }
  | .catalog c => reclamp generic { s with cat := c }

def Inv (generic : List E) (s : St Sel E) : Prop :=
  ∀ e, s.effort = some e → e ∈ allowed generic s

theorem clamp_mem (al : List E) (o : Option E) : ∀ e, clamp al o = some e → e ∈ al := by
  intro e h
  cases o with
  | none => simp [clamp] at h
  | some x =>
    simp only [clamp] at h
    split at h
    · cases h; assumption
    · cases h

theorem inv_reclamp (generic : List E) (s : St Sel E) : Inv generic (reclamp generic s) := by
  intro e h
  exact clamp_mem _ _ e h

theorem inv_step (generic : List E) (s : St Sel E) (ev : Ev Sel E) (h : Inv generic s) :
    Inv generic (step generic s ev) := by
  cases ev with
  | pickEffort o =>
    cases o with
    | none => intro e he; simp [step] at he
    | some x =>
      simp only [step]
      split
      · next hx => intro e he; cases he; exact hx
      · exact h
  | select x => exact inv_reclamp _ _
  | catalog c => exact inv_reclamp _ _

omit [DecidableEq E] in
theorem inv_init (generic : List E) (x : Sel) (c : Sel → Option (List E)) :
    Inv generic ⟨x, none, c⟩ := by
  intro e h; cases h

theorem pick_unoffered_is_ignored (generic : List E) (s : St Sel E) (e : E)
    (h : e ∉ allowed generic s) : step generic s (.pickEffort (some e)) = s := by
  simp [step, h]

theorem clamp_keeps_supported (al : List E) (e : E) (h : e ∈ al) : clamp al (some e) = some e := by
  simp [clamp, h]

/-- Default is always available: choosing it never fails. -/
theorem default_always_available (generic : List E) (s : St Sel E) :
    (step generic s (.pickEffort none)).effort = none := rfl

/-! ## The CLI arguments -/

def effortArgs (toStr : E → String) : Option E → List String
  | none => []
  | some e => ["--effort", toStr e]

omit [DecidableEq E] in
/-- `--effort` is passed exactly when an effort is chosen, and its value comes
from an offered list: if every offered token satisfies `V` (the GUI's
`valid_effort`: non-empty, `[a-z0-9_-]`, no leading `-`), so does the value. -/
theorem effort_args_valid (generic : List E) (toStr : E → String) (V : E → Prop)
    (s : St Sel E) (hgen : ∀ e ∈ generic, V e) (hcat : ∀ x l, s.cat x = some l → ∀ e ∈ l, V e)
    (h : Inv generic s) :
    (effortArgs toStr s.effort = [] ↔ s.effort = none) ∧
      (∀ e, s.effort = some e → effortArgs toStr s.effort = ["--effort", toStr e] ∧ V e) := by
  refine ⟨?_, ?_⟩
  · cases hs : s.effort <;> simp [effortArgs]
  · intro e he
    refine ⟨by simp [effortArgs, he], ?_⟩
    have hm := h e he
    unfold allowed at hm
    cases hc : s.cat s.sel with
    | none => rw [hc] at hm; exact hgen e hm
    | some l => rw [hc] at hm; exact hcat _ _ hc e hm

end Tny.Effort
