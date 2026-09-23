/-!
# Turn and message status (gui/src/turn.rs)

The desktop companion shows three pieces of turn state: the status line under
the composer (`Phase`), the note under the user's message (`UserStatus`) and
the note under tny's reply (`ReplyStatus`). All three are derived from one
machine, `TurnMachine` in `gui/src/turn.rs`, whose transition function is
`stepCur` below. `Export.lean` prints `stepCur` over its whole finite domain to
`golden/turn.tsv`, and a Rust test compares `TurnMachine::step` against every
row, so these theorems are about the transition function the GUI runs.

The bug this machine replaces: the user's message said "Sending…" until the
whole turn (including every tool call) finished, and a stream that ended
without `turn_end` left tny's reply on "Streaming…" forever.
-/

namespace Tny.Turn

inductive Phase where
  | idle | sending | waiting | thinking | writing | tool | approval | saving
  deriving DecidableEq, Repr

inductive UserStatus where
  | none | sending | sent | saved | unconfirmed | discarded
  deriving DecidableEq, Repr

inductive ReplyStatus where
  | absent | streaming | done | stopped | incomplete
  deriving DecidableEq, Repr

/-- The CLI event types the GUI distinguishes (`ask --events=jsonl`). -/
inductive CliKind where
  | thinking | text | toolStart | toolEnd | approval | info | error | turnEndOk | turnEndFail
  deriving DecidableEq, Repr

/-- How a finished turn was reconciled with the saved session (`turn::outcome`). -/
inductive Outcome where
  | saved | savedFailed | unconfirmed | discarded
  deriving DecidableEq, Repr

inductive Ev where
  | submit
  | delivered
  | cli (k : CliKind)
  | finish (o : Outcome)
  deriving DecidableEq, Repr

structure S where
  phase : Phase
  user : UserStatus
  reply : ReplyStatus
  failed : Bool
  deriving DecidableEq, Repr

def init : S := ⟨.idle, .none, .absent, false⟩

def Phase.active : Phase → Bool
  | .idle | .saving => false
  | _ => true

def cliPhase (p : Phase) : CliKind → Phase
  | .thinking => .thinking
  | .text => .writing
  | .toolStart => .tool
  | .toolEnd => .waiting
  | .approval => .approval
  | .info | .error => if p = .sending then .waiting else p
  | .turnEndOk | .turnEndFail => .saving

def cliReply (r : ReplyStatus) : CliKind → ReplyStatus
  | .text => if r = .absent then .streaming else r
  | .turnEndOk => if r = .streaming then .done else r
  | .turnEndFail => if r = .streaming then .stopped else r
  | _ => r

def outcomeUser : Outcome → UserStatus
  | .saved | .savedFailed => .saved
  | .unconfirmed => .unconfirmed
  | .discarded => .discarded

/-- The transition function for events of the current generation. -/
def stepCur (s : S) : Ev → S
  | .submit =>
    if s.phase = .idle then ⟨.sending, .sending, .absent, false⟩ else s
  | .delivered =>
    if s.phase = .sending then { s with phase := .waiting, user := .sent } else s
  | .cli k =>
    if s.phase.active then
      { phase := cliPhase s.phase k
        user := if s.user = .sending then .sent else s.user
        reply := cliReply s.reply k
        failed := s.failed || k = .error || k = .turnEndFail }
    else s
  | .finish o =>
    if s.phase = .idle then s
    else
      { phase := .idle
        user := outcomeUser o
        reply := if s.reply = .streaming then .incomplete else s.reply
        failed := s.failed || o = .savedFailed || o = .discarded }

/-- Every event carries the generation it was issued for; anything from an
older turn or chat is ignored (`generation` checks in `gui/src/main.rs`). -/
structure G where
  gen : Nat
  s : S

inductive GEv where
  | submit
  | ev (g : Nat) (e : Ev)

def step (x : G) : GEv → G
  | .submit =>
    let s' := stepCur x.s .submit
    if s' = x.s then x else ⟨x.gen + 1, s'⟩
  | .ev g e => if g = x.gen then ⟨x.gen, stepCur x.s e⟩ else x

/-! ## Labels: the only strings the GUI shows for these states -/

def userMeta : UserStatus → String
  | .sending => "Sending…"
  | .sent => "Sent"
  | .unconfirmed => "Unconfirmed · inspect saved session"
  | _ => ""

def replyMeta : ReplyStatus → String
  | .streaming => "Streaming…"
  | .stopped => "Stopped before finishing"
  | .incomplete => "Incomplete · the stream ended early"
  | _ => ""

def phaseLabel : Phase → String
  | .idle => ""
  | .sending => "Sending to tny…"
  | .waiting => "Waiting for the model…"
  | .thinking => "Thinking…"
  | .writing => "Writing…"
  | .tool => "Running a tool…"
  | .approval => "Approval requested · unattended CLI policy applies"
  | .saving => "Saving…"

/-! ## Invariant -/

/-- What every reachable state satisfies:
* "Sending…" is shown exactly while the prompt has not reached tny;
* a settled (idle) turn never shows an in-progress note;
* a reply streams only while the turn is live. -/
def Inv (s : S) : Prop :=
  (s.user = .sending ↔ s.phase = .sending) ∧
  (s.phase = .idle → s.user ≠ .sent ∧ s.reply ≠ .streaming) ∧
  (s.phase = .sending → s.reply = .absent) ∧
  (s.phase ≠ .idle → s.user = .sending ∨ s.user = .sent)

instance (s : S) : Decidable (Inv s) := by unfold Inv; infer_instance

def allPhase : List Phase :=
  [.idle, .sending, .waiting, .thinking, .writing, .tool, .approval, .saving]
def allUser : List UserStatus := [.none, .sending, .sent, .saved, .unconfirmed, .discarded]
def allReply : List ReplyStatus := [.absent, .streaming, .done, .stopped, .incomplete]
def allCli : List CliKind :=
  [.thinking, .text, .toolStart, .toolEnd, .approval, .info, .error, .turnEndOk, .turnEndFail]
def allOutcome : List Outcome := [.saved, .savedFailed, .unconfirmed, .discarded]
def allEv : List Ev :=
  [.submit, .delivered] ++ allCli.map .cli ++ allOutcome.map .finish
def allS : List S :=
  allPhase.flatMap fun p => allUser.flatMap fun u => allReply.flatMap fun r =>
    [false, true].map fun f => ⟨p, u, r, f⟩

theorem mem_allS (s : S) : s ∈ allS := by
  rcases s with ⟨p, u, r, f⟩
  simp only [allS, List.mem_flatMap, List.mem_map]
  exact ⟨p, by cases p <;> decide, u, by cases u <;> decide, r, by cases r <;> decide,
    f, by cases f <;> decide, rfl⟩

theorem mem_allEv (e : Ev) : e ∈ allEv := by
  cases e with
  | submit => decide
  | delivered => decide
  | cli k => cases k <;> decide
  | finish o => cases o <;> decide

/-- Any Boolean property of one transition that holds on the full table holds
for every state and event. The table is checked by the kernel (`decide +kernel`),
not by compiled code. -/
theorem of_table {P : S → Ev → Bool}
    (h : allS.all (fun s => allEv.all (P s)) = true) (s : S) (e : Ev) : P s e = true :=
  List.all_eq_true.mp (List.all_eq_true.mp h s (mem_allS s)) e (mem_allEv e)

set_option maxRecDepth 100000 in
/-- All 480 states × 15 events. -/
theorem inv_step_table :
    allS.all (fun s => allEv.all fun e => !decide (Inv s) || decide (Inv (stepCur s e))) = true := by
  decide +kernel

theorem inv_init : Inv init := by decide

theorem inv_stepCur (s : S) (e : Ev) (h : Inv s) : Inv (stepCur s e) := by
  have he := of_table inv_step_table s e
  simp only [Bool.or_eq_true, Bool.not_eq_true', decide_eq_false_iff_not,
    decide_eq_true_eq] at he
  exact he.resolve_left (fun hn => hn h)

theorem inv_step (x : G) (e : GEv) (h : Inv x.s) : Inv (step x e).s := by
  cases e with
  | submit =>
    simp only [step]
    split
    · exact h
    · exact inv_stepCur _ _ h
  | ev g e =>
    simp only [step]
    split
    · exact inv_stepCur _ _ h
    · exact h

inductive Reachable : G → Prop where
  | init : Reachable ⟨0, init⟩
  | step {x} (e : GEv) : Reachable x → Reachable (step x e)

theorem reachable_inv {x : G} (h : Reachable x) : Inv x.s := by
  induction h with
  | init => exact inv_init
  | step e _ ih => exact inv_step _ e ih

/-! ## Guarantees -/

/-- The fixed bug: in every reachable state, "Sending…" is shown under the
user's message only while the prompt is still on its way to tny. -/
theorem sending_label_only_while_sending {x : G} (h : Reachable x) :
    userMeta x.s.user = "Sending…" ↔ x.s.phase = .sending := by
  have hi := (reachable_inv h).1
  rw [← hi]
  cases x.s.user <;> decide

/-- Once tny has the prompt (the write completed, or any CLI event arrived),
the message is no longer "Sending…". -/
theorem delivery_leaves_sending (s : S) (hp : s.phase = .sending) :
    (stepCur s .delivered).user = .sent := by
  simp [stepCur, hp]

theorem any_event_leaves_sending (s : S) (k : CliKind) (hp : s.phase = .sending)
    (hu : s.user = .sending) : (stepCur s (.cli k)).user = .sent := by
  simp [stepCur, hp, hu, Phase.active]

/-- A finished process always settles the turn: nothing is left "Sending…",
"Sent" or "Streaming…", whatever the outcome and whatever state it arrived in. -/
theorem finish_settles (s : S) (o : Outcome) (hp : s.phase ≠ .idle) :
    (stepCur s (.finish o)).phase = .idle ∧ (stepCur s (.finish o)).user ≠ .sending ∧
      (stepCur s (.finish o)).user ≠ .sent ∧ (stepCur s (.finish o)).reply ≠ .streaming := by
  cases o <;> cases hr : s.reply <;> simp [stepCur, hp, hr, outcomeUser]

/-- In a reachable settled state, no in-progress note is visible. -/
theorem idle_shows_no_progress {x : G} (h : Reachable x) (hp : x.s.phase = .idle) :
    userMeta x.s.user ≠ "Sending…" ∧ userMeta x.s.user ≠ "Sent" ∧
      replyMeta x.s.reply ≠ "Streaming…" := by
  have hi := reachable_inv h
  have hu : x.s.user ≠ .sending := fun e => by
    have := hi.1.mp e; rw [hp] at this; cases this
  have ⟨hs, hr⟩ := hi.2.1 hp
  refine ⟨?_, ?_, ?_⟩
  · cases hx : x.s.user <;> simp_all [userMeta]
  · cases hx : x.s.user <;> simp_all [userMeta]
  · cases hx : x.s.reply <;> simp_all [replyMeta]

/-- Events from an older generation never change what is shown. -/
theorem stale_is_noop (x : G) (g : Nat) (e : Ev) (hg : g ≠ x.gen) : step x (.ev g e) = x := by
  simp [step, hg]

/-- A turn cannot start while another one is live. -/
theorem no_overlap (s : S) (hp : s.phase ≠ .idle) : stepCur s .submit = s := by
  simp [stepCur, hp]

/-- Once settled, late CLI events and deliveries change nothing. -/
theorem idle_ignores_stream (s : S) (hp : s.phase = .idle) (k : CliKind) :
    stepCur s (.cli k) = s ∧ stepCur s .delivered = s := by
  simp [stepCur, hp, Phase.active]

/-- The status under the user's message only moves forward during a turn. -/
def UserStatus.rank : UserStatus → Nat
  | .none => 0 | .sending => 1 | .sent => 2
  | .saved | .unconfirmed | .discarded => 3

set_option maxRecDepth 100000 in
theorem user_monotone_table :
    allS.all (fun s => allEv.all fun e =>
      !decide (Inv s) || e == .submit || s.phase == .idle ||
        decide (s.user.rank ≤ (stepCur s e).user.rank)) = true := by
  decide +kernel

theorem user_monotone (s : S) (e : Ev) (he : e ≠ .submit) (h : Inv s)
    (hp : s.phase ≠ .idle) : s.user.rank ≤ (stepCur s e).user.rank := by
  have t := of_table user_monotone_table s e
  simp only [Bool.or_eq_true, Bool.not_eq_true', decide_eq_false_iff_not,
    decide_eq_true_eq, beq_iff_eq] at t
  rcases t with ((hn | he') | hp') | ok
  · exact absurd h hn
  · exact absurd he' he
  · exact absurd hp' hp
  · exact ok

/-! ## Outcome classification (`turn::outcome`) -/

/-- `confirmed`: the saved session was read back; `accepted`: it contains the
new user message; `failed`: the stream or process failed; `hadId`: the stream
named a session. -/
def outcome (confirmed accepted failed hadId : Bool) : Outcome :=
  if confirmed then
    if accepted then (if failed then .savedFailed else .saved)
    else (if failed then .discarded else .unconfirmed)
  else if !hadId && failed then .discarded else .unconfirmed

/-- A message is shown as saved only if the saved session proved it was kept. -/
theorem saved_needs_proof (c a f h : Bool) :
    outcomeUser (outcome c a f h) = .saved → c = true ∧ a = true := by
  cases c <;> cases a <;> cases f <;> cases h <;> decide

/-- A prompt is discarded (and restored to the draft) only after a failure. -/
theorem discard_needs_failure (c a f h : Bool) :
    outcome c a f h = .discarded → f = true := by
  cases c <;> cases a <;> cases f <;> cases h <;> decide

end Tny.Turn
