/-!
# Markdown blocks (gui/src/markdown.rs)

tny's replies are Markdown. The GUI splits a reply into blocks line by line
(`markdown::blocks`) and renders each block with its own layout: headings,
paragraphs, list items and quotes go through Slint's `StyledText` for inline
emphasis, code and links; fenced code and tables are shown verbatim in a
monospace face.

The grouping is a fold over lines driven by `action`, a finite table over
(what block is open, what the line looks like). `Export.lean` prints `action`
to `golden/markdown.tsv` and a Rust test compares `markdown::action` with every
row. How a line is classified (`LineKind`) is left abstract here: the theorems
hold for *any* classifier, so they also hold for the Rust one, which looks at
the open fence to decide whether a line closes it.

Guarantees:
* `roundtrip` — rendering never drops, duplicates or reorders a line of the
  reply: the blocks' source lines concatenate back to the input exactly.
* `blocks_nonempty` — every block has at least one line.
* `fence_opaque`, `unclosed_fence_single_block` — inside a code fence nothing is
  interpreted as Markdown, and a fence still open at the end of a streaming
  reply is one code block, not re-parsed text.
-/

namespace Tny.Markdown

inductive LineKind where
  | blank | fence | heading | rule | item | quote | table | text
  deriving DecidableEq, Repr

inductive BlockKind where
  | gap | heading | rule | para | item | quote | table | code
  deriving DecidableEq, Repr

/-- The kind of block that is still accepting lines. -/
inductive Open where
  | none | para | item | quote | table | fence
  deriving DecidableEq, Repr

inductive Action where
  /-- add the line to the open block -/
  | append
  /-- add the line to the open block, then close it (a closing fence) -/
  | appendClose
  /-- close the open block and open a new one with this line -/
  | start (k : BlockKind)
  /-- close the open block and emit a one-line block -/
  | single (k : BlockKind)
  deriving DecidableEq, Repr

def action : Open → LineKind → Action
  -- Inside a fence only the closing fence means anything.
  | .fence, .fence => .appendClose
  | .fence, _ => .append
  | _, .blank => .single .gap
  | _, .fence => .start .code
  | _, .heading => .single .heading
  | _, .rule => .single .rule
  | _, .item => .start .item
  | .quote, .quote => .append
  | _, .quote => .start .quote
  | .table, .table => .append
  | _, .table => .start .table
  -- Lazy continuation: plain text joins an open paragraph, list item or quote.
  | .para, .text | .item, .text | .quote, .text => .append
  | _, .text => .start .para

structure Block where
  kind : BlockKind
  lines : List String
  deriving DecidableEq, Repr

def openOf : Option Block → Open
  | none => .none
  | some ⟨.para, _⟩ => .para
  | some ⟨.item, _⟩ => .item
  | some ⟨.quote, _⟩ => .quote
  | some ⟨.table, _⟩ => .table
  | some ⟨.code, _⟩ => .fence
  | some _ => .none

def close : Option Block → List Block
  | none => []
  | some b => [b]

structure P where
  done : List Block
  cur : Option Block

def addLine (l : String) (b : Block) : Block := { b with lines := b.lines ++ [l] }

/-- One line. `cls` sees the open block (so a fence closes only with a matching
marker) and the line. -/
def feed (cls : Option Block → String → LineKind) (p : P) (l : String) : P :=
  match action (openOf p.cur) (cls p.cur l) with
  | .append => { p with cur := p.cur.map (addLine l) }
  | .appendClose => ⟨p.done ++ close (p.cur.map (addLine l)), none⟩
  | .start k => ⟨p.done ++ close p.cur, some ⟨k, [l]⟩⟩
  | .single k => ⟨p.done ++ close p.cur ++ [⟨k, [l]⟩], none⟩

def finish (p : P) : List Block := p.done ++ close p.cur

def parse (cls : Option Block → String → LineKind) (ls : List String) : List Block :=
  finish (ls.foldl (feed cls) ⟨[], none⟩)

def source (bs : List Block) : List String := bs.flatMap (·.lines)

/-! ## Round trip -/

/-- With nothing open, no line can be appended to a block that does not exist. -/
theorem action_none (k : LineKind) :
    ∃ b, action .none k = .start b ∨ action .none k = .single b := by
  cases k <;> simp [action]

theorem source_append (a b : List Block) : source (a ++ b) = source a ++ source b := by
  simp [source, List.flatMap_append]

theorem source_close_map (c : Option Block) (l : String) (hc : c ≠ none) :
    source (close (c.map (addLine l))) = source (close c) ++ [l] := by
  rcases c with _ | b
  · exact absurd rfl hc
  · simp [close, source, addLine]

/-- Appending needs an open block; the actions that append only occur when the
open kind is not `none`, and a non-`none` open kind implies a block is open. -/
theorem append_has_block (c : Option Block) (k : LineKind)
    (h : action (openOf c) k = .append ∨ action (openOf c) k = .appendClose) : c ≠ none := by
  intro hc
  subst hc
  rcases action_none k with ⟨b, hb | hb⟩ <;> simp [openOf, hb] at h

theorem feed_source (cls) (p : P) (l : String) :
    source (finish (feed cls p l)) = source (finish p) ++ [l] := by
  unfold feed finish
  split
  · next h =>
    have hc := append_has_block p.cur _ (Or.inl h)
    simp only [source_append, List.append_assoc]
    rw [source_close_map _ _ hc]
  · next h =>
    have hc := append_has_block p.cur _ (Or.inr h)
    show source (p.done ++ close (p.cur.map (addLine l)) ++ close none) =
      source (p.done ++ close p.cur) ++ [l]
    rw [source_append, source_append, source_append, source_close_map _ _ hc]
    simp [close, source]
  · simp [close, source]
  · simp [close, source]

theorem foldl_source (cls) (ls : List String) (p : P) :
    source (finish (ls.foldl (feed cls) p)) = source (finish p) ++ ls := by
  induction ls generalizing p with
  | nil => simp
  | cons l ls ih =>
    rw [List.foldl_cons, ih, feed_source]
    simp

/-- No line of a reply is lost, duplicated or reordered by block rendering. -/
theorem roundtrip (cls) (ls : List String) : source (parse cls ls) = ls := by
  unfold parse
  rw [foldl_source]
  simp [finish, close, source]

/-! ## Every block has content -/

def NonEmpty (p : P) : Prop :=
  (∀ b ∈ p.done, b.lines ≠ []) ∧ (∀ b, p.cur = some b → b.lines ≠ [])

theorem feed_nonempty (cls) (p : P) (l : String) (h : NonEmpty p) : NonEmpty (feed cls p l) := by
  obtain ⟨hd, hc⟩ := h
  unfold feed
  split
  · refine ⟨hd, ?_⟩
    intro b hb
    rcases hcur : p.cur with _ | b0
    · simp [hcur] at hb
    · simp [hcur, addLine] at hb; subst hb; simp
  · refine ⟨?_, by simp⟩
    intro b hb
    simp only [List.mem_append] at hb
    rcases hb with hb | hb
    · exact hd b hb
    · rcases hcur : p.cur with _ | b0
      · simp [hcur, close] at hb
      · simp [hcur, close] at hb; subst hb; simp [addLine]
  · refine ⟨?_, by simp⟩
    intro b hb
    simp only [List.mem_append] at hb
    rcases hb with hb | hb
    · exact hd b hb
    · rcases hcur : p.cur with _ | b0
      · simp [hcur, close] at hb
      · simp [hcur, close] at hb; subst hb; exact hc _ hcur
  · refine ⟨?_, by simp⟩
    intro b hb
    simp only [List.mem_append, List.mem_singleton] at hb
    rcases hb with (hb | hb) | hb
    · exact hd b hb
    · rcases hcur : p.cur with _ | b0
      · simp [hcur, close] at hb
      · simp [hcur, close] at hb; subst hb; exact hc _ hcur
    · subst hb; simp

theorem blocks_nonempty (cls) (ls : List String) : ∀ b ∈ parse cls ls, b.lines ≠ [] := by
  have key : ∀ (ls : List String) (p : P), NonEmpty p → NonEmpty (ls.foldl (feed cls) p) := by
    intro ls
    induction ls with
    | nil => intro p h; exact h
    | cons l ls ih => intro p h; exact ih _ (feed_nonempty cls p l h)
  have h := key ls ⟨[], none⟩ ⟨by simp, by simp⟩
  intro b hb
  unfold parse finish at hb
  simp only [List.mem_append] at hb
  rcases hb with hb | hb
  · exact h.1 b hb
  · rcases hcur : (List.foldl (feed cls) ⟨[], none⟩ ls).cur with _ | b0
    · simp [hcur, close] at hb
    · simp [hcur, close] at hb; subst hb; exact h.2 _ hcur

/-! ## Code fences are opaque -/

/-- Inside a fence, every line except a closing fence is appended verbatim. -/
theorem fence_opaque (k : LineKind) (hk : k ≠ .fence) : action .fence k = .append := by
  cases k <;> simp_all [action]

/-- A fence that is still open when a (streaming) reply ends is a single code
block holding every line from the opening fence on. -/
theorem unclosed_fence_single_block (cls) (first : String) (rest : List String)
    (hopen : cls none first = .fence)
    (hnoclose : ∀ b l, openOf (some b) = .fence → cls (some b) l ≠ .fence) :
    parse cls (first :: rest) = [⟨.code, first :: rest⟩] := by
  have key : ∀ (rest : List String) (acc : List String),
      rest.foldl (feed cls) ⟨[], some ⟨.code, acc⟩⟩ = ⟨[], some ⟨.code, acc ++ rest⟩⟩ := by
    intro rest
    induction rest with
    | nil => intro acc; simp
    | cons l rest ih =>
      intro acc
      rw [List.foldl_cons]
      have hk := hnoclose ⟨.code, acc⟩ l rfl
      have ha : action (openOf (some ⟨.code, acc⟩)) (cls (some ⟨.code, acc⟩) l) = .append := by
        simpa [openOf] using fence_opaque _ hk
      have : feed cls ⟨[], some ⟨.code, acc⟩⟩ l = ⟨[], some ⟨.code, acc ++ [l]⟩⟩ := by
        unfold feed; rw [ha]; simp [addLine]
      rw [this, ih]; simp
  unfold parse
  rw [List.foldl_cons]
  have h0 : feed cls ⟨[], none⟩ first = ⟨[], some ⟨.code, [first]⟩⟩ := by
    unfold feed; simp [openOf, hopen, action, close]
  rw [h0, key]
  simp [finish, close]

/-! ## Finite domain, for the golden table -/

def allOpen : List Open := [.none, .para, .item, .quote, .table, .fence]
def allKind : List LineKind := [.blank, .fence, .heading, .rule, .item, .quote, .table, .text]

theorem mem_allOpen (o : Open) : o ∈ allOpen := by cases o <;> decide
theorem mem_allKind (k : LineKind) : k ∈ allKind := by cases k <;> decide

end Tny.Markdown
