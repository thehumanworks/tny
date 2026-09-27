import Dictation

/-!
Prints the tables the C unit tests replay (tests/test_dictation.c):
`lake exe export golden` rewrites `golden/*.tsv`. CI rebuilds the proofs,
regenerates the tables and fails on any difference, and `make test` replays
the committed tables against the C implementation, so the C code, the tables
and the proven Lean definitions cannot drift apart.

* `lifecycle.tsv` — `Lifecycle.step` on all 180 states × 8 events.
* `dictionaries.tsv` — named dictionaries as the JSON the C parser reads.
* `verify.tsv` — `verify` and `deliver` on generated proposals: every
  order-preserving subset of each sentence's candidate corrections, the same
  subsets with an unlisted edit or in reverse order, and targeted cases for
  every verdict.
-/

open Tny.Dictation

/-! ## Escaping -/

def hex4 (n : Nat) : String :=
  let d := "0123456789abcdef".toList
  String.ofList [d[n / 4096 % 16]!, d[n / 256 % 16]!, d[n / 16 % 16]!, d[n % 16]!]

def jsonStr (s : List Char) : String :=
  "\"" ++ String.join (s.map fun c =>
    if c == '"' then "\\\"" else if c == '\\' then "\\\\"
    else if c == '\n' then "\\n" else if c == '\t' then "\\t" else if c == '\r' then "\\r"
    else if c.toNat < 0x20 || c.toNat == 0x7f || (0x80 ≤ c.toNat && c.toNat ≤ 0x9f)
    then "\\u" ++ hex4 c.toNat
    else c.toString) ++ "\""

def tsvCell (s : List Char) : String :=
  String.join (s.map fun c =>
    if c == '\\' then "\\\\" else if c == '\t' then "\\t" else if c == '\n' then "\\n"
    else if c == '\r' then "\\r" else c.toString)

/-! ## Dictionaries -/

structure Spec where
  word : String
  context : String
  exact : Bool
  aliases : List String

def Spec.entry (s : Spec) : Entry := ⟨s.word.toList, s.exact, s.aliases.map String.toList⟩

/-- Flat `"word": "context"` when nothing else is set, else the object form. -/
def Spec.json (s : Spec) : String :=
  let key := jsonStr s.word.toList
  if !s.exact && s.aliases.isEmpty then key ++ ":" ++ jsonStr s.context.toList
  else
    let fields :=
      (if s.context.isEmpty then [] else ["\"context\":" ++ jsonStr s.context.toList]) ++
      (if s.aliases.isEmpty then []
       else ["\"aliases\":[" ++ ",".intercalate (s.aliases.map (jsonStr ·.toList)) ++ "]"]) ++
      (if s.exact then ["\"case\":\"exact\""] else [])
    key ++ ":{" ++ ",".intercalate fields ++ "}"

def dicts : List (String × List Spec) :=
  [("none", []),
   ("project", [
     ⟨"tny", "the agent harness", true, ["tiny", "tee en why"]⟩,
     ⟨"kubectl", "Kubernetes CLI", false, ["kube cuddle", "cube control"]⟩,
     ⟨"Jev", "decision engine", true, ["jeff"]⟩,
     ⟨".NET", "Microsoft platform", false, ["dot net"]⟩,
     ⟨"Claude Code", "", true, []⟩,
     ⟨"Postgres", "database", false, []⟩])]

def dictOf (name : String) : Dict :=
  ((dicts.find? (·.1 == name)).map fun d => d.2.map Spec.entry).getD []

def dictTable : String :=
  "name\tjson\n" ++ String.join (dicts.map fun (n, specs) =>
    n ++ "\t{" ++ ",".intercalate (specs.map Spec.json) ++ "}\n")

/-! ## Verify cases -/

def reasonName : Reason → String
  | .dictionary => "dictionary" | .case => "case" | .punctuation => "punctuation"
  | .number => "number"

def verdictName : Verdict → String
  | .ok => "ok" | .invalidText => "invalid_text" | .tooMany => "too_many"
  | .emptySpan => "empty_span" | .inadmissible => "inadmissible" | .unlisted => "unlisted"
  | .editBound => "edit_bound"

def proposalJson (p : Proposal) : String :=
  "{\"text\":" ++ jsonStr p.text ++ ",\"corrections\":[" ++
    ",".intercalate (p.corrections.map fun c =>
      "{\"span\":" ++ jsonStr c.span ++ ",\"replacement\":" ++ jsonStr c.repl ++
        ",\"reason\":\"" ++ reasonName c.reason ++ "\"}") ++ "]}"

def c (span repl : String) (r : Reason) : Correction := ⟨span.toList, repl.toList, r⟩

/-- Sentences with candidate corrections in positional order; some candidates
are inadmissible on purpose. -/
def sentences : List (String × String × List Correction) :=
  [("project", "please ask tiny to run kube cuddle on twenty three pods",
     [c "please" "Please" .case, c "ask" "delete" .dictionary, c "tiny" "tny" .dictionary,
      c "run kube" "kubectl" .dictionary, c "kube cuddle" "kubectl" .dictionary,
      c "twenty three" "23" .number, c "pods" "pods." .punctuation]),
   ("project", "hello world its jeff here we shipped one thousand twenty three builds",
     [c "hello" "Hello," .punctuation, c "its" "it's" .punctuation, c "jeff" "jev" .dictionary,
      c "jeff" "Jev" .dictionary, c "one thousand twenty three" "1,023" .number,
      c "builds" "builds." .punctuation]),
   ("project", "version 3.5 costs 1000 dollars and -5 credits",
     [c "version" "Version" .case, c "3.5" "35" .punctuation, c "1000" "1,000" .number,
      c "dollars" "dollars," .punctuation, c "-5" "5" .punctuation, c "credits" "credits." .punctuation]),
   ("project", "check the changes 世界 with dot net and postgres",
     [c "check" "Check" .case, c "世界" "世界," .punctuation, c "dot net" ".NET" .dictionary,
      c "postgres" "Postgres" .dictionary, c "postgres" "Postgres." .case]),
   ("project", "use cloud code for tee en why and not tiny",
     [c "cloud" "Claude Code" .dictionary, c "code" "" .punctuation,
      c "tee en why" "tny" .dictionary, c "not tiny" "tny" .dictionary, c "tiny" "tny." .dictionary]),
   ("none", "ask tiny about twenty-three errors, then stop",
     [c "ask" "Ask" .case, c "tiny" "tny" .dictionary, c "twenty-three" "23" .number,
      c "errors," "errors;" .punctuation, c "stop" "stop." .punctuation]),
   ("project", "tiny tiny tiny tiny tiny",
     [c "tiny" "tny" .dictionary, c "tiny" "tny" .dictionary, c "tiny" "tny" .dictionary,
      c "tiny" "tny" .dictionary, c "tiny" "tny" .dictionary])]

/-- Order-preserving subsets. -/
def subsets {α : Type} : List α → List (List α)
  | [] => [[]]
  | x :: xs => let r := subsets xs; r.map (x :: ·) ++ r

def row (dict : String) (raw : List Char) (p : Proposal) : String :=
  let d := dictOf dict
  s!"{dict}\t{tsvCell raw}\t{proposalJson p}\t{verdictName (verify d raw p)}\t{tsvCell (deliver d raw (some p))}"

def generated : List String :=
  sentences.flatMap fun (dict, raw, cands) =>
    let raw := raw.toList
    (subsets cands).flatMap fun cs =>
      match applyAll raw (cs.map toCorr) with
      | none => [row dict raw ⟨raw, cs⟩]
      | some t =>
        [row dict raw ⟨t, cs⟩, row dict raw ⟨t ++ " extra".toList, cs⟩,
         row dict raw ⟨t.map Char.toUpper, cs⟩] ++
        (if cs.length ≥ 2 then [row dict raw ⟨t, cs.reverse⟩] else [])

def targeted : List String :=
  let r := "tiny runs twenty three tests".toList
  let many := (List.replicate 65 (c "tests" "tests" .case))
  [row "project" r ⟨r, []⟩,
   row "project" r ⟨"".toList, []⟩,
   row "project" r ⟨" \n\t ".toList, []⟩,
   row "project" r ⟨"tny runs \u001b[2J tests".toList, [c "tiny" "tny" .dictionary]⟩,
   row "project" r ⟨"tny runs twenty three tests\u0085".toList, [c "tiny" "tny" .dictionary]⟩,
   row "project" r ⟨r, many⟩,
   row "project" r ⟨r, List.replicate 64 (c "" "" .case)⟩,
   row "project" r ⟨"tny runs twenty three tests".toList, [c "" "tny" .dictionary]⟩,
   row "project" r ⟨"tny runs 23 tests.".toList,
     [c "tiny" "tny" .dictionary, c "twenty three" "23" .number, c "tests" "tests." .punctuation]⟩,
   row "project" r ⟨"tny runs 24 tests".toList,
     [c "tiny" "tny" .dictionary, c "twenty three" "24" .number]⟩,
   row "project" r ⟨"tny runs twenty three tests".toList, [c "tiny" "TNY" .dictionary]⟩,
   row "project" r ⟨"tny runs twenty three tests".toList, [c "tinier" "tny" .dictionary]⟩,
   row "project" r ⟨"Tiny Runs twenty three tests".toList,
     [c "tiny runs" "Tiny Runs" .case]⟩,
   row "project" r ⟨"tiny runs twenty-three tests".toList,
     [c "twenty three" "twenty-three" .punctuation]⟩,
   row "project" r ⟨"tiny runs twentythree tests".toList,
     [c "twenty three" "twentythree" .punctuation]⟩,
   row "project" "we need kubectl and kubectl".toList ⟨"we need kubectl and kubectl".toList,
     [c "kubectl" "kubectl" .dictionary, c "kubectl" "kubectl" .dictionary]⟩,
   row "project" "say \"hi\" \\ to tiny".toList ⟨"say \"hi\" \\ to tny".toList,
     [c "tiny" "tny" .dictionary]⟩,
   row "project" "run it ... now".toList ⟨"run it tny now".toList,
     [c "..." "tny" .dictionary]⟩,
   row "project" "run it tiny, now".toList ⟨"run it tny, now".toList,
     [c "tiny," "tny," .dictionary]⟩,
   row "project" "ask cube control".toList ⟨"ask kubectl".toList,
     [c "cube control" "kubectl" .dictionary]⟩,
   row "project" "ask Cube Control".toList ⟨"ask kubectl".toList,
     [c "Cube Control" "kubectl" .dictionary]⟩,
   row "project" "ask cube controls".toList ⟨"ask kubectl".toList,
     [c "cube controls" "kubectl" .dictionary]⟩,
   row "none" "about 1234, maybe".toList ⟨"about 1,234, maybe".toList,
     [c "1234," "1,234," .number]⟩,
   row "none" "pay 123456 now".toList ⟨"pay 123,456 now".toList,
     [c "123456" "123,456" .number]⟩,
   row "none" "pay 123 now".toList ⟨"pay 0,123 now".toList, [c "123" "0,123" .number]⟩,
   row "none" "pay 1234567 now".toList ⟨"pay 1234,567 now".toList,
     [c "1234567" "1234,567" .number]⟩,
   row "none" "id 999999999999999".toList ⟨"id 999,999,999,999,999".toList,
     [c "999999999999999" "999,999,999,999,999" .number]⟩,
   row "none" "please stop.".toList ⟨"please stop".toList, [c "." "" .punctuation]⟩,
   row "none" "please stop.".toList ⟨"please sto".toList, [c "p." "" .punctuation]⟩,
   row "none" "one".toList ⟨"1".toList, [c "one" "1" .number]⟩,
   row "none" "zero point five".toList ⟨"0 point 5".toList,
     [c "zero" "0" .number, c "five" "5" .number]⟩,
   row "none" "a b c d e f g h".toList ⟨"A B C D E F G H".toList,
     [c "a b c d e f g h" "A B C D E F G H" .case]⟩,
   row "none" "a b c d e f g h".toList ⟨"a b c d e f".toList,
     [c "g h" "" .punctuation]⟩,
   row "none" "one two three four five six seven eight nine ten".toList
     ⟨"1 2 3 4 5 6 7 8 9 10".toList,
      ["one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"].zipIdx.map
        fun (w, i) => c w (toString (i + 1)) .number⟩]

def verifyTable : String :=
  "dict\traw\tproposal\tverdict\tdelivered\n" ++ String.join ((generated ++ targeted).map (· ++ "\n"))

/-! ## Lifecycle -/

namespace Names
open Tny.Dictation.Lifecycle
def phase : Phase → String
  | .transcribing => "transcribing" | .normalizing => "normalizing" | .done => "done"
def outcome : Outcome → String
  | .pending => "pending" | .failed => "failed" | .cancelled => "cancelled" | .raw => "raw"
  | .normalized => "normalized"
def bool (b : Bool) : String := if b then "1" else "0"
def ev : Ev → String
  | .sttDone v => "stt_done:" ++ bool v | .sttFailed => "stt_failed" | .cancel => "cancel"
  | .effortRejected => "effort_rejected" | .normFailed => "norm_failed"
  | .proposal a => "proposal:" ++ bool a
def s (x : S) : String :=
  s!"{phase x.phase}\t{bool x.enabled}\t{bool x.effort}\t{x.requests}\t{outcome x.outcome}"
end Names

def lifecycleTable : String :=
  open Tny.Dictation.Lifecycle in
  "phase\tenabled\teffort\trequests\toutcome\tevent\tphase'\tenabled'\teffort'\trequests'\toutcome'\n" ++
    String.join (allS.flatMap fun s => allEv.map fun e =>
      s!"{Names.s s}\t{Names.ev e}\t{Names.s (step s e)}\n")

def main (args : List String) : IO UInt32 := do
  let dir := args.headD "golden"
  IO.FS.createDirAll dir
  for (name, body) in [("lifecycle.tsv", lifecycleTable), ("dictionaries.tsv", dictTable),
      ("verify.tsv", verifyTable)] do
    IO.FS.writeFile (dir ++ "/" ++ name) body
  return 0
