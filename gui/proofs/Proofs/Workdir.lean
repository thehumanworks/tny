/-!
# Working folder (gui/src/workdir.rs)

The composer's folder control changes the directory tny runs in (`--cwd`).
tny sessions belong to their workspace (`~/.tny/sessions/<workspace-hash>/`;
`--resume` from another folder fails with "no session … for this workspace"),
so a chat cannot move between folders: changing the folder of a chat that
already has a session starts a new chat there, and the old chat stays in the
old folder's history.

Several things are derived from the folder and arrive asynchronously: the CLI
bridge's `--cwd`, the label, the `@` file index and the Recent sessions list.
`Workdir` in Rust keeps them together; `Export.lean` prints `step` over a
two-folder instance to `golden/workdir.tsv` and a Rust test compares every row.

Guarantees (`inv_step`, `reachable_inv` and the theorems below):
* the bridge's `--cwd` and the label always equal the current folder;
* an applied file index or session list always belongs to the current folder
  (results computed for a previous folder are dropped);
* an open chat's session always belongs to the current folder;
* the folder never changes while a turn runs (`busy_freezes_cwd`), so a turn's
  saved session is looked up in the folder it ran in;
* an invalid folder, or any change during a turn, changes nothing.
-/

namespace Tny.Workdir

variable {D : Type} [DecidableEq D]

structure W (D : Type) where
  cwd : D
  bridge : D
  label : D
  index : Option D
  list : Option D
  session : Option D
  busy : Bool
  deriving DecidableEq, Repr

inductive Ev (D : Type) where
  /-- The user picks folder `d`; `valid` is the local check (absolute, existing
  directory, canonical, no control characters). -/
  | change (d : D) (valid : Bool)
  | indexArrived (d : D)
  | listArrived (d : D)
  /-- Open a chat from the Recent list. -/
  | select
  | submit
  /-- The turn ended; the CLI saved its session in the folder it ran in. -/
  | finish
  | newChat
  deriving DecidableEq, Repr

def step (s : W D) : Ev D → W D
  | .change d valid =>
    if s.busy || !valid then s
    else { cwd := d, bridge := d, label := d, index := none, list := none,
           session := none, busy := false }
  | .indexArrived d => if d = s.cwd then { s with index := some d } else s
  | .listArrived d => if d = s.cwd then { s with list := some d } else s
  | .select =>
    if s.busy then s
    else match s.list with
      | some d => if d = s.cwd then { s with session := some d } else s
      | none => s
  | .submit => if s.busy then s else { s with busy := true }
  | .finish => if s.busy then { s with busy := false, session := some s.bridge } else s
  | .newChat => if s.busy then s else { s with session := none }

def Inv (s : W D) : Prop :=
  s.bridge = s.cwd ∧ s.label = s.cwd ∧
  (s.index = none ∨ s.index = some s.cwd) ∧
  (s.list = none ∨ s.list = some s.cwd) ∧
  (s.session = none ∨ s.session = some s.cwd)

instance (s : W D) : Decidable (Inv s) := by unfold Inv; infer_instance

def init (d : D) : W D := ⟨d, d, d, none, none, none, false⟩

omit [DecidableEq D] in
theorem inv_init (d : D) : Inv (init d) := by
  simp [Inv, init]

theorem inv_step (s : W D) (e : Ev D) (h : Inv s) : Inv (step s e) := by
  obtain ⟨hb, hl, hi, hs, hn⟩ := h
  cases e with
  | change d valid =>
    simp only [step]
    split
    · exact ⟨hb, hl, hi, hs, hn⟩
    · simp [Inv]
  | indexArrived d =>
    simp only [step]
    split
    · next hd => exact ⟨hb, hl, Or.inr (by rw [hd]), hs, hn⟩
    · exact ⟨hb, hl, hi, hs, hn⟩
  | listArrived d =>
    simp only [step]
    split
    · next hd => exact ⟨hb, hl, hi, Or.inr (by rw [hd]), hn⟩
    · exact ⟨hb, hl, hi, hs, hn⟩
  | select =>
    simp only [step]
    split
    · exact ⟨hb, hl, hi, hs, hn⟩
    · split
      · split
        · next hd => exact ⟨hb, hl, hi, hs, Or.inr (by simp [hd])⟩
        · exact ⟨hb, hl, hi, hs, hn⟩
      · exact ⟨hb, hl, hi, hs, hn⟩
  | submit =>
    simp only [step]
    split
    · exact ⟨hb, hl, hi, hs, hn⟩
    · exact ⟨hb, hl, hi, hs, hn⟩
  | finish =>
    simp only [step]
    split
    · exact ⟨hb, hl, hi, hs, Or.inr (by simp [hb])⟩
    · exact ⟨hb, hl, hi, hs, hn⟩
  | newChat =>
    simp only [step]
    split
    · exact ⟨hb, hl, hi, hs, hn⟩
    · exact ⟨hb, hl, hi, hs, Or.inl rfl⟩

inductive Reachable : W D → Prop where
  | init (d : D) : Reachable (init d)
  | step {s} (e : Ev D) : Reachable s → Reachable (step s e)

theorem reachable_inv {s : W D} (h : Reachable s) : Inv s := by
  induction h with
  | init d => exact inv_init d
  | step e _ ih => exact inv_step _ e ih

/-- The folder never changes while a turn runs. -/
theorem busy_freezes_cwd (s : W D) (e : Ev D) (h : s.busy = true) : (step s e).cwd = s.cwd := by
  cases e <;> simp [step, h] <;> split <;> simp

/-- The session a finished turn opens is the folder the turn ran in. -/
theorem finish_opens_in_cwd {s : W D} (hr : Reachable s) (h : s.busy = true) :
    (step s .finish).session = some s.cwd := by
  have hb := (reachable_inv hr).1
  simp [step, h, hb]

/-- Invalid input, or any change while busy, is ignored entirely. -/
theorem rejected_change_is_noop (s : W D) (d : D) (valid : Bool)
    (h : s.busy = true ∨ valid = false) : step s (.change d valid) = s := by
  rcases h with h | h <;> simp [step, h]

/-- A valid change on an idle chat lands on the new folder, with every derived
value reset until it is recomputed for that folder. -/
theorem change_lands (s : W D) (d : D) (h : s.busy = false) :
    step s (.change d true) = ⟨d, d, d, none, none, none, false⟩ := by
  simp [step, h]

/-- A result computed for another folder is never applied. -/
theorem stale_results_dropped (s : W D) (d : D) (h : d ≠ s.cwd) :
    step s (.indexArrived d) = s ∧ step s (.listArrived d) = s := by
  simp [step, h]

/-! ## Two-folder instance for the golden table -/

def allW : List (W Bool) :=
  let o : List (Option Bool) := [none, some false, some true]
  [false, true].flatMap fun c => [false, true].flatMap fun b => [false, true].flatMap fun l =>
    o.flatMap fun i => o.flatMap fun li => o.flatMap fun se =>
      [false, true].map fun bu => ⟨c, b, l, i, li, se, bu⟩

def allEv : List (Ev Bool) :=
  [false, true].flatMap (fun d => [false, true].map fun v => Ev.change d v) ++
  [false, true].map Ev.indexArrived ++ [false, true].map Ev.listArrived ++
  [.select, .submit, .finish, .newChat]

end Tny.Workdir
