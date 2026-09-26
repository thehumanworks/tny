/-!
# Applying listed corrections (`apply_corrections` in src/core/dictation_verify.c)

The normalizer model returns a rewritten transcript together with the list of
edits it claims to have made. The verifier rebuilds the rewrite from the raw
transcript and that list alone, so an edit the model did not list cannot
survive (the reconstruction would differ from the returned text).

Corrections are applied left to right. Each span is matched at its **first**
occurrence at or after the end of the previous match (a cursor), in bytes for
C and in characters here; for valid UTF-8 the two agree because a UTF-8
pattern can only match at a character boundary.

Guarantees:
* `applyAll_sound` — whenever application succeeds, raw and result share the
  same gaps in the same order and differ only at the listed spans (`Decomp`);
  every character of the result outside a replacement is a character of the
  raw transcript at the corresponding position, so there are no unlisted
  edits.
* `applyAll_nil` — no corrections reproduce the raw transcript exactly.
* `noop_preserves` — corrections whose replacement equals their span cannot
  change the text.
-/

namespace Tny.Dictation

structure Corr (α : Type) where
  span : List α
  repl : List α
  deriving DecidableEq, Repr

variable {α : Type} [DecidableEq α]

/-- First occurrence of `pat` in `s` as `(before, after)`. -/
def findSplit (pat : List α) : List α → Option (List α × List α)
  | [] => if pat.isPrefixOf [] then some ([], []) else none
  | c :: cs =>
    if pat.isPrefixOf (c :: cs) then some ([], (c :: cs).drop pat.length)
    else (findSplit pat cs).map fun pr => (c :: pr.1, pr.2)

theorem prefix_split {pat s : List α} (h : pat.isPrefixOf s = true) :
    s = pat ++ s.drop pat.length :=
  (List.prefix_iff_eq_append.mp (List.isPrefixOf_iff_prefix.mp h)).symm

theorem findSplit_spec {pat : List α} :
    ∀ {s p r : List α}, findSplit pat s = some (p, r) → s = p ++ pat ++ r
  | [], p, r, h => by
    simp only [findSplit] at h
    split at h
    · next hp =>
      simp only [Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      have := prefix_split hp
      simp only [List.nil_append, List.append_nil]
      simpa using this
    · exact absurd h (by simp)
  | c :: cs, p, r, h => by
    simp only [findSplit] at h
    split at h
    · next hp =>
      simp only [Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      simpa using prefix_split hp
    · cases hs : findSplit pat cs with
      | none => simp [hs] at h
      | some pr =>
        obtain ⟨p', r'⟩ := pr
        simp only [hs, Option.map_some, Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨rfl, rfl⟩ := h
        simp [findSplit_spec hs]

/-- Apply corrections in order, each after the previous match. -/
def applyAll : List α → List (Corr α) → Option (List α)
  | s, [] => some s
  | s, c :: cs =>
    match findSplit c.span s with
    | none => none
    | some (p, r) => (applyAll r cs).map fun t => p ++ c.repl ++ t

/-- `Decomp raw text cs`: both texts are the same gaps interleaved with the
listed spans (raw) and replacements (text), in order. -/
inductive Decomp : List α → List α → List (Corr α) → Prop
  | nil (g : List α) : Decomp g g []
  | cons (g : List α) (c : Corr α) {s t : List α} {cs : List (Corr α)} :
      Decomp s t cs → Decomp (g ++ c.span ++ s) (g ++ c.repl ++ t) (c :: cs)

theorem applyAll_nil (s : List α) : applyAll s [] = some s := rfl

theorem applyAll_sound : ∀ {s t : List α} {cs : List (Corr α)},
    applyAll s cs = some t → Decomp s t cs
  | s, t, [], h => by
    simp only [applyAll, Option.some.injEq] at h
    subst h
    exact .nil s
  | s, t, c :: cs, h => by
    simp only [applyAll] at h
    split at h
    · exact absurd h (by simp)
    · next p r hsplit =>
      cases hr : applyAll r cs with
      | none => simp [hr] at h
      | some u =>
        simp only [hr, Option.map_some, Option.some.injEq] at h
        subst h
        have hs := findSplit_spec hsplit
        subst hs
        exact .cons p c (applyAll_sound hr)

omit [DecidableEq α] in
/-- Replacing each span by itself changes nothing. -/
theorem decomp_noop : ∀ {s t : List α} {cs : List (Corr α)},
    Decomp s t cs → (∀ c ∈ cs, c.repl = c.span) → t = s
  | _, _, _, .nil _, _ => rfl
  | _, _, _, .cons g c d, h => by
    have hc := h c (by simp)
    have ht := decomp_noop d (fun x hx => h x (by simp [hx]))
    rw [hc, ht]

theorem noop_preserves {s t : List α} {cs : List (Corr α)} (h : applyAll s cs = some t)
    (hn : ∀ c ∈ cs, c.repl = c.span) : t = s :=
  decomp_noop (applyAll_sound h) hn

omit [DecidableEq α] in
/-- The raw text is recovered from a decomposition by putting the spans back. -/
theorem decomp_symm : ∀ {s t : List α} {cs : List (Corr α)},
    Decomp s t cs → Decomp t s (cs.map fun c => ⟨c.repl, c.span⟩)
  | _, _, _, .nil g => .nil g
  | _, _, _, .cons g c d => .cons g ⟨c.repl, c.span⟩ (decomp_symm d)

end Tny.Dictation
