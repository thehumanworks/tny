import Proofs

/-!
Prints the finite transition tables the Rust tests replay (`gui/src/*`):
`lake exe export golden` rewrites `golden/*.tsv`. CI regenerates them and
fails on any difference, so the Rust code, the tables and the proven Lean
definitions cannot drift apart.
-/

open Tny

namespace Names
open Turn in
def phase : Phase → String
  | .idle => "idle" | .sending => "sending" | .waiting => "waiting" | .thinking => "thinking"
  | .writing => "writing" | .tool => "tool" | .approval => "approval" | .saving => "saving"
open Turn in
def user : UserStatus → String
  | .none => "none" | .sending => "sending" | .sent => "sent" | .saved => "saved"
  | .unconfirmed => "unconfirmed" | .discarded => "discarded"
open Turn in
def reply : ReplyStatus → String
  | .absent => "absent" | .streaming => "streaming" | .done => "done" | .stopped => "stopped"
  | .incomplete => "incomplete"
open Turn in
def cli : CliKind → String
  | .thinking => "thinking" | .text => "text" | .toolStart => "tool_start"
  | .toolEnd => "tool_end" | .approval => "approval" | .info => "info" | .error => "error"
  | .turnEndOk => "turn_end_ok" | .turnEndFail => "turn_end_fail"
open Turn in
def outcome : Outcome → String
  | .saved => "saved" | .savedFailed => "saved_failed" | .unconfirmed => "unconfirmed"
  | .discarded => "discarded"
open Turn in
def ev : Ev → String
  | .submit => "submit" | .delivered => "delivered"
  | .cli k => "cli:" ++ cli k | .finish o => "finish:" ++ outcome o
def bool (b : Bool) : String := if b then "1" else "0"
open Turn in
def s (x : S) : String :=
  s!"{phase x.phase}\t{user x.user}\t{reply x.reply}\t{bool x.failed}"

open Markdown in
def openK : Open → String
  | .none => "none" | .para => "para" | .item => "item" | .quote => "quote"
  | .table => "table" | .fence => "fence"
open Markdown in
def line : LineKind → String
  | .blank => "blank" | .fence => "fence" | .heading => "heading" | .rule => "rule"
  | .item => "item" | .quote => "quote" | .table => "table" | .text => "text"
open Markdown in
def block : BlockKind → String
  | .gap => "gap" | .heading => "heading" | .rule => "rule" | .para => "para"
  | .item => "item" | .quote => "quote" | .table => "table" | .code => "code"
open Markdown in
def action : Action → String
  | .append => "append" | .appendClose => "append_close"
  | .start k => "start:" ++ block k | .single k => "single:" ++ block k

def dir (b : Bool) : String := if b then "B" else "A"
def odir : Option Bool → String | none => "-" | some b => dir b
open Workdir in
def w (x : W Bool) : String :=
  s!"{dir x.cwd}\t{odir x.index}\t{odir x.list}\t{odir x.session}\t{bool x.busy}"
open Workdir in
def wev : Ev Bool → String
  | .change d v => s!"change:{dir d}:{bool v}" | .indexArrived d => s!"index:{dir d}"
  | .listArrived d => s!"list:{dir d}" | .select => "select" | .submit => "submit"
  | .finish => "finish" | .newChat => "new_chat"
end Names

def lines (header : String) (rows : List String) : String :=
  String.intercalate "\n" (header :: rows) ++ "\n"

/-- Only states satisfying `Turn.Inv`: every reachable state does
(`Turn.reachable_inv`), so agreeing on these rows covers every state the GUI can reach. -/
def turnTable : String :=
  lines "phase\tuser\treply\tfailed\tevent\tphase'\tuser'\treply'\tfailed'" <|
    (Turn.allS.filter fun s => decide (Turn.Inv s)).flatMap fun s => Turn.allEv.map fun e =>
      s!"{Names.s s}\t{Names.ev e}\t{Names.s (Turn.stepCur s e)}"

def turnLabels : String :=
  lines "kind\tstate\tlabel" <|
    Turn.allPhase.map (fun p => s!"phase\t{Names.phase p}\t{Turn.phaseLabel p}") ++
    Turn.allUser.map (fun u => s!"user\t{Names.user u}\t{Turn.userMeta u}") ++
    Turn.allReply.map (fun r => s!"reply\t{Names.reply r}\t{Turn.replyMeta r}")

def outcomeTable : String :=
  let bs := [false, true]
  lines "confirmed\taccepted\tfailed\thad_id\toutcome" <|
    bs.flatMap fun c => bs.flatMap fun a => bs.flatMap fun f => bs.map fun h =>
      s!"{Names.bool c}\t{Names.bool a}\t{Names.bool f}\t{Names.bool h}\t{Names.outcome (Turn.outcome c a f h)}"

def markdownTable : String :=
  lines "open\tline\taction" <|
    Markdown.allOpen.flatMap fun o => Markdown.allKind.map fun k =>
      s!"{Names.openK o}\t{Names.line k}\t{Names.action (Markdown.action o k)}"

/-- Only states satisfying `Workdir.Inv` (all reachable ones, `Workdir.reachable_inv`).
There `bridge` and `label` equal `cwd`, which `Workdir.inv_step` keeps true, so
they are not printed: the Rust side derives both from `cwd`. -/
def workdirTable : String :=
  lines "cwd\tindex\tlist\tsession\tbusy\tevent\tcwd'\tindex'\tlist'\tsession'\tbusy'" <|
    (Workdir.allW.filter fun s => decide (Workdir.Inv s)).flatMap fun s => Workdir.allEv.map fun e =>
      s!"{Names.w s}\t{Names.wev e}\t{Names.w (Workdir.step s e)}"

def main (args : List String) : IO UInt32 := do
  let dir := args.headD "golden"
  IO.FS.createDirAll dir
  for (name, body) in [("turn.tsv", turnTable), ("turn_labels.tsv", turnLabels),
      ("turn_outcome.tsv", outcomeTable), ("markdown.tsv", markdownTable),
      ("workdir.tsv", workdirTable)] do
    IO.FS.writeFile (dir ++ "/" ++ name) body
  return 0
