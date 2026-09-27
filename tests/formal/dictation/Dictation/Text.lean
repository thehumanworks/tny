/-!
# Text classification shared by the verifier (src/core/dictation_verify.c)

Everything here is ASCII-only classification on characters. C classifies
bytes; for valid UTF-8 the two agree because every byte of a multi-byte
character is ≥ 0x80 and is therefore a "word" byte in both (never space,
punctuation, digit or an ASCII letter to fold).

* `tokens` — split on ASCII whitespace, dropping empty pieces.
* `semTokens` — the words that carry meaning: ASCII letters folded to lower
  case and ASCII punctuation removed, except that a token containing a digit
  keeps its inner punctuation (so `3.5` never equals `35`, `-5` never
  equals `5`) and only loses sentence punctuation at its edges.
* `numeralVal` / `numberVal` — the only numbers a `number` correction may
  convert: plain or comma-grouped digits, and English cardinals below 10^12
  (`zero`, `twenty-three`, `one hundred and five`, `two million forty`).
* `lev` — Levenshtein distance, the spec for the C banded implementation.
-/

namespace Tny.Dictation

def isSpace (c : Char) : Bool := c == ' ' || c == '\t' || c == '\n' || c == '\r'

def isPunct (c : Char) : Bool :=
  let n := c.toNat
  (33 ≤ n && n ≤ 47) || (58 ≤ n && n ≤ 64) || (91 ≤ n && n ≤ 96) || (123 ≤ n && n ≤ 126)

def isDigit (c : Char) : Bool := '0' ≤ c && c ≤ '9'

/-- Sentence, quote and bracket punctuation: removable at a token's edge. -/
def isEdge (c : Char) : Bool := ".,;:!?\"'()[]{}".toList.contains c

def fold (s : List Char) : List Char := s.map Char.toLower

def trimBy (p : Char → Bool) (s : List Char) : List Char :=
  ((s.dropWhile p).reverse.dropWhile p).reverse

def tokensAux : List Char → List Char → List (List Char)
  | [], cur => if cur.isEmpty then [] else [cur.reverse]
  | c :: cs, cur =>
    if isSpace c then (if cur.isEmpty then tokensAux cs [] else cur.reverse :: tokensAux cs [])
    else tokensAux cs (c :: cur)

def tokens (s : List Char) : List (List Char) := tokensAux s []

def normTok (t : List Char) : List Char :=
  if t.any isDigit then fold (trimBy isEdge t) else fold (t.filter fun c => !isPunct c)

def semTokens (s : List Char) : List (List Char) :=
  ((tokens s).map normTok).filter fun t => !t.isEmpty

/-! ## Transcript validity (`tny_dictation_text_valid`) -/

def isControl (c : Char) : Bool :=
  (c.toNat < 0x20 && !(c == '\t' || c == '\n' || c == '\r')) || c.toNat == 0x7f ||
    (0x80 ≤ c.toNat && c.toNat ≤ 0x9f)

def utf8Size (s : List Char) : Nat := (s.map Char.utf8Size).sum

def textMax : Nat := 65536

def textValid (s : List Char) : Bool :=
  !s.all isSpace && utf8Size s ≤ textMax && !s.any isControl

/-! ## Numbers -/

def digitVal (c : Char) : Nat := c.toNat - '0'.toNat

def digitsVal (ds : List Char) : Nat := ds.foldl (fun n c => n * 10 + digitVal c) 0

def splitComma : List Char → List (List Char)
  | [] => [[]]
  | c :: cs =>
    let rest := splitComma cs
    if c == ',' then [] :: rest else (c :: rest.head!) :: rest.tail

/-- Plain digits (no leading zero, at most 15) or `d{1,3}(,ddd)+`. -/
def numeralVal (s : List Char) : Option Nat :=
  match splitComma s with
  | [] => none
  | [g] =>
    if !g.isEmpty && g.all isDigit && g.length ≤ 15 && (g.length == 1 || g.head? != some '0')
    then some (digitsVal g) else none
  | g :: gs =>
    let ds := g ++ gs.flatten
    if !g.isEmpty && g.length ≤ 3 && g.head? != some '0' && gs.all (·.length == 3) &&
        ds.all isDigit && ds.length ≤ 15
    then some (digitsVal ds) else none

def unitWords : List String :=
  ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
   "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
   "nineteen"]
def tensWords : List String :=
  ["twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"]
def scaleWords : List String := ["thousand", "million", "billion"]

def indexIn (w : List Char) : List String → Nat → Option Nat
  | [], _ => none
  | x :: xs, i => if x.toList == w then some i else indexIn w xs (i + 1)

def unitOf (w : List Char) : Option Nat := indexIn w unitWords 0
def tensOf (w : List Char) : Option Nat := (indexIn w tensWords 0).map fun i => 20 + 10 * i
/-- `thousand` = 1, `million` = 2, `billion` = 3. -/
def scaleOf (w : List Char) : Option Nat := (indexIn w scaleWords 0).map (· + 1)

/-- `0..19`, or a tens word optionally followed by `1..9` (greedy). -/
def below100 : List (List Char) → Option (Nat × List (List Char))
  | [] => none
  | w :: rest =>
    match unitOf w with
    | some u => some (u, rest)
    | none =>
      match tensOf w with
      | none => none
      | some t =>
        match rest with
        | w2 :: rest2 =>
          match unitOf w2 with
          | some u => if 1 ≤ u && u ≤ 9 then some (t + u, rest2) else some (t, rest)
          | none => some (t, rest)
        | [] => some (t, [])

/-- `U hundred [[and] N]` with `U` in 1..9 and `N` ≥ 1, else `below100`. -/
def below1000 : List (List Char) → Option (Nat × List (List Char))
  | w :: h :: rest =>
    match unitOf w with
    | some u =>
      if 1 ≤ u && u ≤ 9 && h == "hundred".toList then
        let (hadAnd, rest') := match rest with
          | a :: r => if a == "and".toList then (true, r) else (false, rest)
          | [] => (false, [])
        match below100 rest' with
        | some (v, r) => if 1 ≤ v then some (100 * u + v, r)
                         else if hadAnd then none else some (100 * u, rest)
        | none => if hadAnd then none else some (100 * u, rest)
      else below100 (w :: h :: rest)
    | none => below100 (w :: h :: rest)
  | ws => below100 ws

/-- Groups with strictly decreasing scales; every group is at least one. -/
def groups (ws : List (List Char)) (limit : Nat) : Option Nat :=
  match below1000 ws with
  | none => none
  | some (v, rest) =>
    if v = 0 then none
    else match rest with
      | [] => some v
      | s :: rest2 =>
        match scaleOf s with
        | none => none
        | some k =>
          if h : k < limit then
            if rest2.isEmpty then some (v * 1000 ^ k)
            else (groups rest2 k).map fun x => v * 1000 ^ k + x
          else none
termination_by limit

def wordsVal (ws : List (List Char)) : Option Nat :=
  if ws == ["zero".toList] then some 0 else groups ws 4

def numCore (s : List Char) : List Char := trimBy (fun c => isEdge c || isSpace c) s

def numberVal (s : List Char) : Option Nat :=
  let c := numCore s
  match numeralVal c with
  | some v => some v
  | none => wordsVal (tokens (fold (c.map fun ch => if ch == '-' then ' ' else ch)))

/-- A `number` correction: the replacement is a numeral with the value the
span spells (in words or as a differently grouped numeral). -/
def numberOk (span repl : List Char) : Bool :=
  match numeralVal (numCore repl), numberVal span with
  | some a, some b => a == b
  | _, _ => false

/-! ## Token edit distance -/

def lev {α : Type} [DecidableEq α] : List α → List α → Nat
  | [], ys => ys.length
  | x :: xs, [] => xs.length + 1
  | x :: xs, y :: ys =>
    min (min (lev xs (y :: ys) + 1) (lev (x :: xs) ys + 1)) (lev xs ys + if x = y then 0 else 1)
termination_by xs ys => xs.length + ys.length

theorem lev_self {α : Type} [DecidableEq α] : ∀ xs : List α, lev xs xs = 0
  | [] => by simp [lev]
  | x :: xs => by
    rw [lev]
    simp [lev_self xs]

/-! ### Row-by-row evaluation

`lev` as written is the specification but takes exponential time. `levDP`
computes one DP row per element of the first list (the distances from that
suffix to every suffix of the second list); `levDP_eq` proves it equal and
`@[csimp]` makes compiled code (the golden-table exporter) use it. -/

def rowStep {α : Type} [DecidableEq α] (x : α) (n : Nat) : List α → List Nat → List Nat
  | [], _ => [n]
  | y :: ys, o0 :: o1 :: os =>
    match rowStep x n ys (o1 :: os) with
    | [] => []
    | n1 :: rest => min (min (o0 + 1) (n1 + 1)) (o1 + if x = y then 0 else 1) :: n1 :: rest
  | _ :: _, _ => []

/-- `l`, then each proper suffix down to `[]`. -/
def suffixes {α : Type} : List α → List (List α)
  | [] => [[]]
  | x :: xs => (x :: xs) :: suffixes xs

def row {α : Type} [DecidableEq α] : List α → List α → List Nat
  | [], ys => (suffixes ys).map List.length
  | x :: xs, ys => rowStep x (xs.length + 1) ys (row xs ys)

def levDP {α : Type} [DecidableEq α] (xs ys : List α) : Nat := (row xs ys).headD 0

theorem suffixes_head {α : Type} (l : List α) : suffixes l = l :: (suffixes l).tail := by
  cases l <;> simp [suffixes]

theorem rowStep_spec {α : Type} [DecidableEq α] (x : α) (xs : List α) :
    ∀ ys : List α, rowStep x (xs.length + 1) ys ((suffixes ys).map (lev xs)) =
      (suffixes ys).map (lev (x :: xs))
  | [] => by
    simp only [suffixes, List.map_cons, List.map_nil, rowStep, List.cons.injEq, and_true]
    rw [lev.eq_2]
  | y :: ys => by
    have ih := rowStep_spec x xs ys
    have hs := suffixes_head ys
    rw [hs] at ih
    simp only [List.map_cons] at ih
    simp only [suffixes]
    rw [hs]
    simp only [List.map_cons, rowStep, ih, List.cons.injEq, and_true]
    rw [lev.eq_3]

theorem row_spec {α : Type} [DecidableEq α] :
    ∀ xs ys : List α, row xs ys = (suffixes ys).map (lev xs)
  | [], ys => by
    simp only [row]
    congr 1
    funext zs
    cases zs <;> simp [lev]
  | x :: xs, ys => by
    simp only [row, row_spec xs ys]
    exact rowStep_spec x xs ys

theorem levDP_eq {α : Type} [DecidableEq α] (xs ys : List α) : levDP xs ys = lev xs ys := by
  simp only [levDP, row_spec]
  rw [suffixes_head ys]
  simp

@[csimp] theorem lev_eq_levDP : @lev = @levDP := by
  funext α _ xs ys
  exact (levDP_eq xs ys).symm

/-- Allowed semantic token edits for a transcript of `n` semantic tokens. -/
def editBound (n : Nat) : Nat := 3 + n / 4

end Tny.Dictation
