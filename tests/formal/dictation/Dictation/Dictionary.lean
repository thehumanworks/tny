import Dictation.Text

/-!
# User dictionary (src/core/dictation_dictionary.c)

`~/.tny/dictionary.json` (user) and `<workspace>/.tny/dictionary.json`
(project) are merged with the project winning per word, the `settings.json`
convention. The merged dictionary lists the project entries first, then every
user entry whose word the project does not define.

Guarantees:
* `merge_lookup` — looking a word up in the merge is the project entry when
  there is one, otherwise the user entry.
* `merge_words` — the merge defines exactly the words of the two files.
* `merge_nodup` — merging duplicate-free files stays duplicate-free.
-/

namespace Tny.Dictation

structure Entry where
  word : List Char
  exact : Bool
  aliases : List (List Char)
  deriving DecidableEq, Repr

abbrev Dict := List Entry

def merge (user project : Dict) : Dict :=
  project ++ user.filter fun e => !project.any (·.word == e.word)

def lookup (d : Dict) (w : List Char) : Option Entry := d.find? (·.word == w)

theorem find_filter_irrelevant (u : Dict) (q : Entry → Bool) (w : List Char)
    (h : ∀ e ∈ u, e.word = w → q e = true) :
    (u.filter q).find? (·.word == w) = u.find? (·.word == w) := by
  induction u with
  | nil => rfl
  | cons e es ih =>
    have ih' := ih (fun x hx => h x (by simp [hx]))
    by_cases hw : e.word = w
    · have hq := h e (by simp) hw
      simp [List.filter, hq, List.find?, hw]
    · have hb : (e.word == w) = false := by simp [hw]
      by_cases hq : q e = true
      · simp [List.filter, hq, List.find?, hb, ih']
      · simp [List.filter, hq, List.find?, hb, ih']

theorem merge_lookup (u p : Dict) (w : List Char) :
    lookup (merge u p) w = (lookup p w).or (lookup u w) := by
  unfold lookup merge
  rw [List.find?_append]
  cases hp : p.find? (·.word == w) with
  | some e => simp
  | none =>
    simp only [Option.none_or]
    apply find_filter_irrelevant
    intro e _ hw
    have hnone := List.find?_eq_none.mp hp
    simp only [Bool.not_eq_true', List.any_eq_false, beq_iff_eq]
    intro x hx hxw
    have := hnone x hx
    simp [hxw, hw] at this

def words (d : Dict) : List (List Char) := d.map (·.word)

theorem merge_words (u p : Dict) (w : List Char) :
    w ∈ words (merge u p) ↔ w ∈ words u ∨ w ∈ words p := by
  unfold words merge
  simp only [List.map_append, List.mem_append, List.mem_map, List.mem_filter,
    Bool.not_eq_true', List.any_eq_false, beq_iff_eq]
  constructor
  · rintro (⟨e, he, rfl⟩ | ⟨e, ⟨he, _⟩, rfl⟩)
    · exact Or.inr ⟨e, he, rfl⟩
    · exact Or.inl ⟨e, he, rfl⟩
  · rintro (⟨e, he, rfl⟩ | ⟨e, he, rfl⟩)
    · by_cases hp : ∃ x ∈ p, x.word = e.word
      · obtain ⟨x, hx, hxw⟩ := hp
        exact Or.inl ⟨x, hx, hxw⟩
      · refine Or.inr ⟨e, ⟨he, ?_⟩, rfl⟩
        intro x hx hxw
        exact hp ⟨x, hx, hxw⟩
    · exact Or.inl ⟨e, he, rfl⟩

theorem merge_nodup (u p : Dict) (hu : (words u).Nodup) (hp : (words p).Nodup) :
    (words (merge u p)).Nodup := by
  unfold words merge
  rw [List.map_append, List.nodup_append]
  refine ⟨hp, List.Nodup.sublist (List.filter_sublist.map _) hu, ?_⟩
  intro a ha b hb hab
  subst hab
  simp only [List.mem_map, List.mem_filter, Bool.not_eq_true', List.any_eq_false,
    beq_iff_eq] at ha hb
  obtain ⟨x, hx, rfl⟩ := ha
  obtain ⟨y, ⟨_, hy⟩, hyw⟩ := hb
  exact hy x hx hyw.symm

/-! ## Dictionary corrections -/

def dictCore (s : List Char) : List Char := trimBy (fun c => isEdge c || isSpace c) s

/-- The replacement, without surrounding sentence punctuation, is the word:
byte-exact for `"case": "exact"`, ASCII case-insensitive otherwise. -/
def wordMatches (e : Entry) (r : List Char) : Bool :=
  if e.exact then dictCore r == dictCore e.word else fold (dictCore r) == fold (dictCore e.word)

/-- The span has meaning and is a single word, or is one of the entry's known
mishearings (compared as semantic tokens). A multi-word span without an
alias would let a dictionary correction delete words. -/
def spanOk (e : Entry) (span : List Char) : Bool :=
  !(semTokens span).isEmpty &&
    ((tokens span).length == 1 || e.aliases.any fun a => semTokens a == semTokens span)

def dictOk (d : Dict) (span repl : List Char) : Bool :=
  d.any fun e => wordMatches e repl && spanOk e span

theorem dictOk_nil (span repl : List Char) : dictOk [] span repl = false := rfl

end Tny.Dictation
