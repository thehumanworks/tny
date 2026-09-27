import Std

namespace Tny.Acp

def identity : String := "@agentclientprotocol/claude-agent-acp"
def maxComponent : Nat := 4294967295

structure Version where
  major : Nat
  minor : Nat
  patch : Nat
  deriving DecidableEq, Repr

def minimum : Version := ⟨0, 75, 1⟩

def LE (a b : Version) : Prop :=
  a.major < b.major ∨ (a.major = b.major ∧
    (a.minor < b.minor ∨ (a.minor = b.minor ∧ a.patch ≤ b.patch)))
instance (a b : Version) : Decidable (LE a b) := inferInstanceAs (Decidable (_ ∨ _))

theorem le_trans {a b c : Version} (ab : LE a b) (bc : LE b c) : LE a c := by
  unfold LE at *
  omega

/-- The production C comparison is the specialization of lexicographic order
    to the supported minimum; the parser separately enforces uint32 bounds. -/
theorem floor_iff (v : Version) : LE minimum v ↔
    v.major > 0 ∨ v.minor > 75 ∨ (v.minor = 75 ∧ v.patch ≥ 1) := by
  change (0 < v.major ∨ (0 = v.major ∧
    (75 < v.minor ∨ (75 = v.minor ∧ 1 ≤ v.patch)))) ↔ _
  omega

def digit (c : Char) : Bool := '0' ≤ c && c ≤ '9'
def buildChar (c : Char) : Bool :=
  digit c || ('a' ≤ c && c ≤ 'z') || ('A' ≤ c && c ≤ 'Z') || c == '-'

def component (s : String) : Option Nat := do
  if s.isEmpty || !s.toList.all digit then none else do
    if s.length > 1 && s.startsWith "0" then none else do
      let n ← s.toNat?
      if n ≤ maxComponent then some n else none

def core (s : String) : Option Version := do
  match s.splitOn "." with
  | [a, b, c] => return ⟨← component a, ← component b, ← component c⟩
  | _ => none

def validBuild (s : String) : Bool :=
  (s.splitOn ".").all fun p => !p.isEmpty && p.toList.all buildChar

/-- Split exactly once: additional plus signs and empty build identifiers fail. -/
def parseParts : List String → Option Version
  | [s] => core s
  | [s, metadata] => if validBuild metadata then core s else none
  | _ => none

def parse (s : String) : Option Version := parseParts (s.splitOn "+")

def admit (name version : Option String) : Bool :=
  match name, version with
  | some n, some s => n == identity && (parse s).any (fun v => decide (LE minimum v))
  | _, _ => false

theorem exact_identity {n s : Option String} (h : admit n s = true) :
    n = some identity := by
  cases n with
  | none => simp [admit] at h
  | some n =>
    cases s with
    | none => simp [admit] at h
    | some s =>
      simp only [admit, Bool.and_eq_true, beq_iff_eq] at h
      exact congrArg some h.1

theorem parsed_acceptance {s : String} {v : Version} (h : parse s = some v) :
    admit (some identity) (some s) = decide (LE minimum v) := by
  simp [admit, h]

theorem old_rejected {s : String} {v : Version} (h : parse s = some v)
    (old : ¬ LE minimum v) : admit (some identity) (some s) = false := by
  simp [parsed_acceptance h, old]

theorem stable_accepted {s : String} {v : Version} (h : parse s = some v)
    (newer : LE minimum v) : admit (some identity) (some s) = true := by
  simp [parsed_acceptance h, newer]

set_option maxHeartbeats 2000000 in
theorem minimum_accepted : admit (some identity) (some "0.75.1") = true := by cbv

theorem upward_closed {s t : String} {a b : Version}
    (hs : parse s = some a) (ht : parse t = some b)
    (accepted : admit (some identity) (some s) = true) (ab : LE a b) :
    admit (some identity) (some t) = true := by
  apply stable_accepted ht
  apply le_trans (b := a) _ ab
  simpa [parsed_acceptance hs] using accepted

/-- For every valid build, the executable parser returns the identical tuple. -/
theorem build_ignored (s metadata : String) (h : validBuild metadata = true) :
    parseParts [s, metadata] = parseParts [s] := by simp [parseParts, h]

theorem build_order_unchanged (s metadata : String) (h : validBuild metadata = true)
    (bound : Version) :
    (parseParts [s, metadata]).any (fun v => decide (LE bound v)) =
      (parseParts [s]).any (fun v => decide (LE bound v)) := by rw [build_ignored s metadata h]

theorem missing_rejected (s : Option String) :
    admit none s = false ∧ admit s none = false := by cases s <;> simp [admit]

/-- Authority is an admitted handshake, not a persistent cache. Tools-only is
    mandatory in this model; there is no external-built-in tool execution. -/
structure Authority where
  name : Option String
  version : Option String
  protocol : Nat
  loadSession : Bool
  toolsOnly : Bool
  admitted : admit name version = true
  protocolOne : protocol = 1
  restricted : toolsOnly = true

inductive State where
  | disconnected
  | initialized (auth : Authority)
  | ready (auth : Authority)
  | prompting (auth : Authority)

def initHandshake (name version : Option String) (protocol : Nat) (load : Bool) : State :=
  if ha : admit name version = true then
    if hp : protocol = 1 then .initialized ⟨name, version, protocol, load, true, ha, hp, rfl⟩
    else .disconnected
  else .disconnected

inductive Event where
  | connect (name version : Option String) (protocol : Nat) (load : Bool)
  | newSession
  | resume
  | prompt
  | disconnect

/-- Every connect replaces all prior authority, even if invoked while active. -/
def step (s : State) : Event → State
  | .connect n v p l => initHandshake n v p l
  | .disconnect => .disconnected
  | .newSession => match s with
    | .initialized a => .ready a
    | _ => s
  | .resume => match s with
    | .initialized a => if a.loadSession then .ready a else .disconnected
    | _ => s
  | .prompt => match s with
    | .ready a => .prompting a
    | _ => s

def authority : State → Option Authority
  | .disconnected => none
  | .initialized a | .ready a | .prompting a => some a

def Safe (s : State) : Prop := ∀ a, authority s = some a →
  admit a.name a.version = true ∧ a.protocol = 1 ∧ a.toolsOnly = true

inductive Reachable : State → Prop where
  | start : Reachable .disconnected
  | next {s : State} (h : Reachable s) (e : Event) : Reachable (step s e)

/-- A proof-carrying authority makes invalid active states unrepresentable. -/
theorem reachable_safe {s : State} (_ : Reachable s) : Safe s := by
  intro a _
  exact ⟨a.admitted, a.protocolOne, a.restricted⟩

theorem disconnect_clears (s : State) : authority (step s .disconnect) = none := rfl

theorem reconnect_revalidates (s : State) (n v : Option String) (p : Nat) (l : Bool) :
    step s (.connect n v p l) = initHandshake n v p l := rfl

theorem failed_admission_clears (s : State) (n v : Option String) (p : Nat) (l : Bool)
    (h : admit n v = false) : step s (.connect n v p l) = .disconnected := by
  simp [step, initHandshake, h]

theorem bad_protocol_rejected (n v : Option String) (p : Nat) (l : Bool) (h : p ≠ 1) :
    initHandshake n v p l = .disconnected := by simp [initHandshake, h]

def sessionEvent : Event → Prop
  | .newSession | .resume | .prompt | .disconnect => True
  | .connect .. => False

/-- No session/prompt sequence after rejection can regain authority. A fresh,
    successful connect is necessary. This quantifies over arbitrary traces. -/
theorem disconnected_stays (events : List Event) (h : ∀ e ∈ events, sessionEvent e) :
    events.foldl step .disconnected = .disconnected := by
  induction events with
  | nil => rfl
  | cons e es ih =>
    have he := h e (by simp)
    have hs : step .disconnected e = .disconnected := by
      cases e <;> simp_all [sessionEvent, step]
    simp only [List.foldl_cons, hs]
    exact ih (fun e he => h e (by simp [he]))

theorem failed_cannot_prompt (s : State) (n v : Option String) (p : Nat) (l : Bool)
    (h : admit n v = false) (events : List Event)
    (he : ∀ e ∈ events, sessionEvent e) :
    events.foldl step (step s (.connect n v p l)) = .disconnected := by
  rw [failed_admission_clears s n v p l h]
  exact disconnected_stays events he

theorem resume_requires_load (a : Authority) (h : a.loadSession = false) :
    step (.initialized a) .resume = .disconnected := by simp [step, h]

def handshake (n v : Option String) (p : Nat) (load resume : Bool) : Bool :=
  match step (initHandshake n v p load) (if resume then .resume else .newSession) with
  | .ready _ => true
  | _ => false

/-- Any successfully parsed positive major is admitted, without a major ceiling
beyond the parser's uint32 component bound. -/
theorem positive_major_accepted {s : String} {v : Version}
    (h : parse s = some v) (major : 0 < v.major) :
    admit (some identity) (some s) = true := by
  apply stable_accepted h
  exact Or.inl major

theorem parsed_build_ignored (raw base metadata : String)
    (split : raw.splitOn "+" = [base, metadata]) (valid : validBuild metadata = true) :
    parse raw = core base := by simp [parse, split, parseParts, valid]

theorem identity_change_clears (s : State) (n v : Option String) (p : Nat) (l : Bool)
    (changed : n ≠ some identity) : step s (.connect n v p l) = .disconnected := by
  apply failed_admission_clears
  cases h : admit n v with
  | false => rfl
  | true => exact False.elim (changed (exact_identity h))

theorem downgrade_clears (s : State) (raw : String) (v : Version) (p : Nat) (l : Bool)
    (parsed : parse raw = some v) (old : ¬ LE minimum v) :
    step s (.connect (some identity) (some raw) p l) = .disconnected := by
  exact failed_admission_clears s _ _ p l (old_rejected parsed old)

end Tny.Acp
