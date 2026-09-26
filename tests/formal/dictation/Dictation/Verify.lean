import Dictation.Apply
import Dictation.Dictionary

/-!
# The model proposes, C disposes (`tny_dictation_verify` in src/core/dictation_verify.c)

The normalizer model answers `{"text": …, "corrections": [{"span", "replacement",
"reason"}]}`. `verify` accepts that proposal only when every check passes;
`deliver` then hands the frontend either the accepted rewrite or the raw
transcript, never anything else. Checks, in order (the first failure is the
reported verdict):

1. the rewrite passes `tny_dictation_text_valid` (nonblank, ≤ 64 KiB UTF-8, no
   terminal controls);
2. at most 64 corrections;
3. each correction, in order, has a nonempty span and satisfies its declared
   reason (`admissible`);
4. applying the listed corrections to the raw transcript reproduces the
   rewrite exactly (no unlisted edits);
5. the semantic-token Levenshtein distance is at most `3 + n / 4` for `n`
   semantic raw tokens.

Guarantees:
* `verify_ok_sound` — an accepted rewrite is valid, is the raw transcript with
  exactly the listed, admissible replacements (`Decomp`), and is within the
  edit bound;
* `identity_ok` — returning the transcript unchanged with no corrections is
  always accepted, so a well-behaved model is never penalised;
* `deliver_raw_or_verified`, `deliver_valid` — fail open: the delivered text
  is the raw transcript unless an accepted rewrite exists, and it is valid
  whenever the raw transcript was;
* `case_only_preserves_folded` — a rewrite that lists only case corrections
  differs from the raw transcript only in ASCII letter case;
* `empty_dictionary_rejects_dictionary` — without a dictionary no word can be
  replaced by a `dictionary` correction.
-/

namespace Tny.Dictation

inductive Reason | dictionary | case | punctuation | number
  deriving DecidableEq, Repr

structure Correction where
  span : List Char
  repl : List Char
  reason : Reason
  deriving DecidableEq, Repr

structure Proposal where
  text : List Char
  corrections : List Correction
  deriving DecidableEq, Repr

def admissible (d : Dict) (c : Correction) : Bool :=
  match c.reason with
  | .dictionary => dictOk d c.span c.repl
  | .case => fold c.span == fold c.repl
  | .punctuation => semTokens c.span == semTokens c.repl
  | .number => numberOk c.span c.repl

inductive Verdict | ok | invalidText | tooMany | emptySpan | inadmissible | unlisted | editBound
  deriving DecidableEq, Repr

def maxCorrections : Nat := 64

def corrVerdict (d : Dict) (c : Correction) : Option Verdict :=
  if c.span.isEmpty then some .emptySpan
  else if !admissible d c then some .inadmissible
  else none

def toCorr (c : Correction) : Corr Char := ⟨c.span, c.repl⟩

def verify (d : Dict) (raw : List Char) (p : Proposal) : Verdict :=
  if !textValid p.text then .invalidText
  else if maxCorrections < p.corrections.length then .tooMany
  else match p.corrections.findSome? (corrVerdict d) with
    | some v => v
    | none =>
      if applyAll raw (p.corrections.map toCorr) != some p.text then .unlisted
      else if editBound (semTokens raw).length < lev (semTokens raw) (semTokens p.text)
      then .editBound
      else .ok

theorem verify_ok_sound {d : Dict} {raw : List Char} {p : Proposal}
    (h : verify d raw p = .ok) :
    textValid p.text = true ∧
    p.corrections.length ≤ maxCorrections ∧
    (∀ c ∈ p.corrections, c.span ≠ [] ∧ admissible d c = true) ∧
    Decomp raw p.text (p.corrections.map toCorr) ∧
    lev (semTokens raw) (semTokens p.text) ≤ editBound (semTokens raw).length := by
  unfold verify at h
  by_cases hv : textValid p.text = true
  · simp only [hv, Bool.not_true, Bool.false_eq_true, ↓reduceIte] at h
    by_cases hm : maxCorrections < p.corrections.length
    · simp [hm] at h
    · simp only [hm, ↓reduceIte] at h
      cases hf : p.corrections.findSome? (corrVerdict d) with
      | some v =>
        rw [hf] at h
        subst h
        have := List.exists_of_findSome?_eq_some hf
        obtain ⟨c, _, hc⟩ := this
        unfold corrVerdict at hc
        split at hc
        · simp at hc
        · split at hc <;> simp at hc
      | none =>
        rw [hf] at h
        simp only at h
        by_cases ha : (applyAll raw (p.corrections.map toCorr) != some p.text) = true
        · simp [ha] at h
        · simp only [ha, Bool.false_eq_true, ↓reduceIte] at h
          have happ : applyAll raw (p.corrections.map toCorr) = some p.text := by
            simpa using ha
          by_cases hb : editBound (semTokens raw).length <
              lev (semTokens raw) (semTokens p.text)
          · simp [hb] at h
          · refine ⟨hv, Nat.le_of_not_lt hm, ?_, applyAll_sound happ, Nat.le_of_not_lt hb⟩
            intro c hc
            have hn := List.findSome?_eq_none_iff.mp hf c hc
            unfold corrVerdict at hn
            by_cases he : c.span.isEmpty = true
            · simp [he] at hn
            · by_cases had : admissible d c = true
              · exact ⟨fun hc' => he (by simp [hc']), had⟩
              · simp [he, had] at hn
  · simp [hv] at h

theorem identity_ok (d : Dict) {raw : List Char} (h : textValid raw = true) :
    verify d raw ⟨raw, []⟩ = .ok := by
  simp [verify, h, applyAll, lev_self, maxCorrections]

def deliver (d : Dict) (raw : List Char) : Option Proposal → List Char
  | none => raw
  | some p => if verify d raw p = .ok then p.text else raw

theorem deliver_raw_or_verified (d : Dict) (raw : List Char) (r : Option Proposal) :
    deliver d raw r = raw ∨
      ∃ p, r = some p ∧ verify d raw p = .ok ∧ deliver d raw r = p.text := by
  cases r with
  | none => exact Or.inl rfl
  | some p =>
    by_cases hv : verify d raw p = .ok
    · exact Or.inr ⟨p, rfl, hv, by simp [deliver, hv]⟩
    · exact Or.inl (by simp [deliver, hv])

theorem deliver_valid (d : Dict) {raw : List Char} (r : Option Proposal)
    (h : textValid raw = true) : textValid (deliver d raw r) = true := by
  rcases deliver_raw_or_verified d raw r with he | ⟨p, _, hv, he⟩
  · rw [he]; exact h
  · rw [he]; exact (verify_ok_sound hv).1

/-- Every delivered text is the raw transcript with some admissible, listed
replacements applied (possibly none). -/
theorem deliver_decomp (d : Dict) (raw : List Char) (r : Option Proposal) :
    ∃ cs : List Correction, (∀ c ∈ cs, admissible d c = true) ∧
      Decomp raw (deliver d raw r) (cs.map toCorr) := by
  rcases deliver_raw_or_verified d raw r with he | ⟨p, _, hv, he⟩
  · exact ⟨[], by simp, by rw [he]; exact .nil raw⟩
  · have s := verify_ok_sound hv
    exact ⟨p.corrections, fun c hc => (s.2.2.1 c hc).2, by rw [he]; exact s.2.2.2.1⟩

theorem decomp_fold : ∀ {s t : List Char} {cs : List (Corr Char)},
    Decomp s t cs → (∀ c ∈ cs, fold c.span = fold c.repl) → fold s = fold t
  | _, _, _, .nil _, _ => rfl
  | _, _, _, .cons g c d, h => by
    have hc := h c (by simp)
    have hd := decomp_fold d (fun x hx => h x (by simp [hx]))
    simp only [fold, List.map_append] at hc hd ⊢
    rw [hc, hd]

theorem case_only_preserves_folded {d : Dict} {raw : List Char} {p : Proposal}
    (h : verify d raw p = .ok) (hc : ∀ c ∈ p.corrections, c.reason = .case) :
    fold raw = fold p.text := by
  have s := verify_ok_sound h
  apply decomp_fold s.2.2.2.1
  intro c hmem
  obtain ⟨c', hc', rfl⟩ := List.mem_map.mp hmem
  have hadm := (s.2.2.1 c' hc').2
  simp only [admissible, hc c' hc', beq_iff_eq] at hadm
  exact hadm

theorem empty_dictionary_rejects_dictionary (span repl : List Char) :
    admissible [] ⟨span, repl, .dictionary⟩ = false := rfl

end Tny.Dictation
