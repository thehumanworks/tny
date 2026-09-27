/-!
# Normalization lifecycle (`tny_dictation_norm_step` in src/core/dictation_verify.c)

Recording is unchanged from ADR 0079 and not modelled. After transcription the
service either finishes (normalization off, or the transcript is invalid) or
enters **Normalizing** and makes one structured rewrite request. A rejected
reasoning-effort field is retried once without the field; any other failure,
a rejected proposal, or cancellation delivers the raw transcript. `dictation.c`
drives every transition through the C translation of `step`, and the C unit
test replays `golden/lifecycle.tsv` (all 180 states × 8 events) against it.

Guarantees (`inv_step`, `reachable_inv` and the theorems below):
* normalization off ⇒ no normalizer request is ever made (`off_never_requests`);
* at most two normalizer requests per transcript (`at_most_two_requests`);
* once a valid transcript exists the outcome can only be raw or normalized:
  normalization never fails, cancels or loses the transcript
  (`transcript_never_lost`);
* cancelling while normalizing delivers the raw transcript
  (`cancel_normalizing_delivers_raw`); so do failures and rejected proposals;
* a rejected effort value is retried without the field exactly once
  (`effort_retry_once`), never a failed normalization;
* a normalized outcome needs normalization enabled and an accepted proposal
  (`normalized_needs_acceptance`);
* every normalizer event makes progress (`normalizer_events_progress`) and
  `done` is absorbing.
-/

namespace Tny.Dictation.Lifecycle

inductive Phase | transcribing | normalizing | done
  deriving DecidableEq, Repr

inductive Outcome | pending | failed | cancelled | raw | normalized
  deriving DecidableEq, Repr

structure S where
  phase : Phase
  enabled : Bool
  /-- The in-flight normalizer request carries the reasoning-effort field. -/
  effort : Bool
  /-- Normalizer requests started for this transcript. -/
  requests : Nat
  outcome : Outcome
  deriving DecidableEq, Repr

inductive Ev
  /-- STT finished; `valid` is `tny_dictation_text_valid` of the transcript. -/
  | sttDone (valid : Bool)
  /-- STT failed or was rejected (exit 1/2). -/
  | sttFailed
  /-- Esc, Ctrl-C, SIGINT/SIGTERM, or an approval taking the keyboard. -/
  | cancel
  /-- HTTP 400/422 while the effort field was sent. -/
  | effortRejected
  /-- No credential, transport error, other HTTP status, timeout, malformed
  output: normalization did not produce a proposal. -/
  | normFailed
  /-- A parsed proposal; `accepted` is `verify … = .ok`. -/
  | proposal (accepted : Bool)
  deriving DecidableEq, Repr

def init (enabled : Bool) : S := ⟨.transcribing, enabled, false, 0, .pending⟩

def step (s : S) : Ev → S
  | .sttDone valid =>
    if s.phase ≠ .transcribing then s
    else if !valid then { s with phase := .done, outcome := .failed }
    else if s.enabled then { s with phase := .normalizing, effort := true, requests := 1 }
    else { s with phase := .done, outcome := .raw }
  | .sttFailed =>
    if s.phase = .transcribing then { s with phase := .done, outcome := .failed } else s
  | .cancel =>
    match s.phase with
    | .transcribing => { s with phase := .done, outcome := .cancelled }
    | .normalizing => { s with phase := .done, outcome := .raw }
    | .done => s
  | .effortRejected =>
    if s.phase = .normalizing then
      if s.effort && s.requests < 2 then { s with effort := false, requests := s.requests + 1 }
      else { s with phase := .done, outcome := .raw }
    else s
  | .normFailed =>
    if s.phase = .normalizing then { s with phase := .done, outcome := .raw } else s
  | .proposal accepted =>
    if s.phase = .normalizing then
      { s with phase := .done, outcome := if accepted then .normalized else .raw }
    else s

def Inv (s : S) : Prop :=
  s.requests ≤ 2 ∧
  (s.enabled = false → s.requests = 0) ∧
  (s.phase = .transcribing → s.requests = 0 ∧ s.outcome = .pending) ∧
  (s.phase = .normalizing →
    s.outcome = .pending ∧ ((s.effort = true ∧ s.requests = 1) ∨ (s.effort = false ∧ s.requests = 2))) ∧
  (s.phase = .done → s.outcome ≠ .pending) ∧
  (1 ≤ s.requests → s.outcome = .pending ∨ s.outcome = .raw ∨ s.outcome = .normalized) ∧
  (s.outcome = .normalized → s.enabled = true ∧ 1 ≤ s.requests)

theorem inv_init (e : Bool) : Inv (init e) := by
  simp [Inv, init]

theorem inv_step (s : S) (e : Ev) (h : Inv s) : Inv (step s e) := by
  obtain ⟨h1, h2, h3, h4, h5, h6, h7⟩ := h
  cases e with
  | sttDone valid =>
    simp only [step]
    by_cases hp : s.phase = .transcribing
    · have ⟨hr, ho⟩ := h3 hp
      cases valid <;> cases s.enabled <;> simp [Inv, hp, hr, ho]
    · simp only [hp, ne_eq, not_false_eq_true, ↓reduceIte]
      exact ⟨h1, h2, h3, h4, h5, h6, h7⟩
  | sttFailed =>
    simp only [step]
    by_cases hp : s.phase = .transcribing
    · have ⟨hr, _⟩ := h3 hp
      simp [Inv, hp, hr]
    · simp only [hp, ↓reduceIte]
      exact ⟨h1, h2, h3, h4, h5, h6, h7⟩
  | cancel =>
    simp only [step]
    cases hp : s.phase with
    | transcribing =>
      have ⟨hr, _⟩ := h3 hp
      simp [Inv, hr]
    | normalizing =>
      have ⟨_, hre⟩ := h4 hp
      simp only [Inv]
      refine ⟨h1, h2, by simp, by simp, by simp, by simp, by simp⟩
    | done => simp only; exact ⟨h1, h2, h3, h4, h5, h6, h7⟩
  | effortRejected =>
    simp only [step]
    by_cases hp : s.phase = .normalizing
    · have ⟨ho, hre⟩ := h4 hp
      simp only [hp, ↓reduceIte]
      rcases hre with ⟨he, hr⟩ | ⟨he, hr⟩
      · simp only [he, hr, Bool.true_and, decide_eq_true_eq, show (1 : Nat) < 2 by omega,
          ↓reduceIte]
        refine ⟨by simp, ?_, by simp, by simp [ho], by simp, by simp [ho], by simp [ho]⟩
        intro hen; have := h2 hen; omega
      · simp only [he, Bool.false_and, Bool.false_eq_true, ↓reduceIte]
        refine ⟨h1, h2, by simp, by simp, by simp, by simp, by simp⟩
    · simp only [hp, ↓reduceIte]
      exact ⟨h1, h2, h3, h4, h5, h6, h7⟩
  | normFailed =>
    simp only [step]
    by_cases hp : s.phase = .normalizing
    · simp only [hp, ↓reduceIte]
      refine ⟨h1, h2, by simp, by simp, by simp, by simp, by simp⟩
    · simp only [hp, ↓reduceIte]
      exact ⟨h1, h2, h3, h4, h5, h6, h7⟩
  | proposal accepted =>
    simp only [step]
    by_cases hp : s.phase = .normalizing
    · have ⟨_, hre⟩ := h4 hp
      have hreq : 1 ≤ s.requests := by rcases hre with ⟨_, h⟩ | ⟨_, h⟩ <;> omega
      have hen : s.enabled = true := by
        cases hx : s.enabled
        · have := h2 hx; omega
        · rfl
      simp only [hp, ↓reduceIte]
      cases accepted <;>
        refine ⟨h1, h2, by simp, by simp, by simp, by simp, ?_⟩ <;> simp [hen, hreq]
    · simp only [hp, ↓reduceIte]
      exact ⟨h1, h2, h3, h4, h5, h6, h7⟩

inductive Reachable : S → Prop
  | init (e : Bool) : Reachable (init e)
  | step {s} (e : Ev) : Reachable s → Reachable (step s e)

theorem reachable_inv {s : S} (h : Reachable s) : Inv s := by
  induction h with
  | init e => exact inv_init e
  | step e _ ih => exact inv_step _ e ih

theorem off_never_requests {s : S} (h : Reachable s) (hoff : s.enabled = false) :
    s.requests = 0 ∧ s.phase ≠ .normalizing ∧ s.outcome ≠ .normalized := by
  have ⟨_, h2, _, h4, _, _, h7⟩ := reachable_inv h
  have hr := h2 hoff
  refine ⟨hr, ?_, ?_⟩
  · intro hp; rcases (h4 hp).2 with ⟨_, h⟩ | ⟨_, h⟩ <;> omega
  · intro ho; have := (h7 ho).1; simp [hoff] at this

theorem at_most_two_requests {s : S} (h : Reachable s) : s.requests ≤ 2 :=
  (reachable_inv h).1

theorem transcript_never_lost {s : S} (h : Reachable s) (hr : 1 ≤ s.requests) :
    s.outcome = .pending ∨ s.outcome = .raw ∨ s.outcome = .normalized :=
  (reachable_inv h).2.2.2.2.2.1 hr

theorem cancel_normalizing_delivers_raw (s : S) (hp : s.phase = .normalizing) :
    (step s .cancel).phase = .done ∧ (step s .cancel).outcome = .raw := by
  simp [step, hp]

theorem failure_delivers_raw (s : S) (hp : s.phase = .normalizing) :
    (step s .normFailed).outcome = .raw ∧ (step s (.proposal false)).outcome = .raw := by
  simp [step, hp]

theorem effort_retry_once {s : S} (h : Reachable s) (hp : s.phase = .normalizing) :
    (s.effort = true → (step s .effortRejected).phase = .normalizing ∧
        (step s .effortRejected).effort = false ∧ (step s .effortRejected).requests = 2) ∧
    (s.effort = false → (step s .effortRejected).phase = .done ∧
        (step s .effortRejected).outcome = .raw) := by
  have hre := ((reachable_inv h).2.2.2.1 hp).2
  constructor
  · intro he
    rcases hre with ⟨_, hr⟩ | ⟨he', _⟩
    · simp [step, hp, he, hr]
    · simp [he] at he'
  · intro he
    simp [step, hp, he]

theorem normalized_needs_acceptance (s : S) (e : Ev) (hs : s.outcome ≠ .normalized)
    (h : (step s e).outcome = .normalized) :
    s.phase = .normalizing ∧ e = .proposal true := by
  cases e with
  | sttDone v =>
    simp only [step] at h
    split at h
    · exact absurd h hs
    · split at h
      · simp at h
      · split at h <;> simp_all
  | sttFailed => simp only [step] at h; split at h <;> simp_all
  | cancel => simp only [step] at h; split at h <;> simp_all
  | effortRejected =>
    simp only [step] at h; split at h
    · split at h <;> simp_all
    · simp_all
  | normFailed => simp only [step] at h; split at h <;> simp_all
  | proposal a =>
    simp only [step] at h
    split at h
    · next hp => cases a <;> simp_all
    · simp_all

theorem done_absorbing (s : S) (e : Ev) (h : s.phase = .done) : step s e = s := by
  cases e <;> simp [step, h]

/-- Remaining normalizer work: two requests with the effort field, one without. -/
def measure (s : S) : Nat :=
  match s.phase with
  | .transcribing => 3
  | .normalizing => if s.effort then 2 else 1
  | .done => 0

theorem normalizer_events_progress (s : S) (hp : s.phase = .normalizing) (e : Ev)
    (he : e = .cancel ∨ e = .effortRejected ∨ e = .normFailed ∨ ∃ a, e = .proposal a) :
    measure (step s e) < measure s := by
  rcases he with rfl | rfl | rfl | ⟨a, rfl⟩
  · simp [step, hp, measure]; split <;> simp
  · simp only [step, hp, ↓reduceIte]
    split
    · next hc =>
      simp only [Bool.and_eq_true, decide_eq_true_eq] at hc
      simp [measure, hp, hc.1]
    · simp [measure, hp]; split <;> simp
  · simp [step, hp, measure]; split <;> simp
  · simp [step, hp, measure]; split <;> simp

/-! ## Finite instance for the golden table -/

def allPhase : List Phase := [.transcribing, .normalizing, .done]
def allOutcome : List Outcome := [.pending, .failed, .cancelled, .raw, .normalized]
def allS : List S :=
  allPhase.flatMap fun p => [false, true].flatMap fun en => [false, true].flatMap fun ef =>
    [0, 1, 2].flatMap fun r => allOutcome.map fun o => ⟨p, en, ef, r, o⟩
def allEv : List Ev :=
  [.sttDone false, .sttDone true, .sttFailed, .cancel, .effortRejected, .normFailed,
   .proposal false, .proposal true]

end Tny.Dictation.Lifecycle
