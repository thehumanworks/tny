import Std

/-! Model selection contract for the ACP client (`ac_set_requested_model`).

The adapter's catalog lags model releases, so it informs `models` output but
never gates a request: the agent is the sole authority on whether a model ID is
served. The client prompts only with the exact model the agent confirmed. -/

namespace Tny.Acp.Model

/-- What the session advertised for model selection. -/
inductive Selector where
  /-- No model select option and no legacy `models` object. -/
  | absent
  /-- A model select option without an `options` array. -/
  | malformed
  /-- `configOptions` model select; `session/set_config_option`. -/
  | config (catalog : List String)
  /-- Older `models.availableModels`; `session/set_model`. -/
  | legacy (catalog : List String)
  deriving DecidableEq, Repr

/-- The agent's answer to the selection request. For `config` the value is the
returned `currentValue`; a legacy acknowledgement carries none. -/
inductive Reply where
  | rejected
  | confirmed (value : String)
  deriving DecidableEq, Repr

inductive Outcome where
  | keepDefault
  | prompt (model : String)
  /-- `annotated`: the error names the model as absent from the catalog. -/
  | fail (annotated : Bool)
  deriving DecidableEq, Repr

def listed : Selector → String → Bool
  | .config c, w | .legacy c, w => c.contains w
  | _, _ => false

/-- Whether a selection request reaches the agent. -/
def sends : Selector → Option String → Bool
  | .config _, some _ | .legacy _, some _ => true
  | _, _ => false

def select (s : Selector) (wanted : Option String) (r : Reply) : Outcome :=
  match wanted with
  | none => .keepDefault
  | some w =>
    match s, r with
    | .absent, _ | .malformed, _ => .fail false
    | .config c, .confirmed v => if v = w then .prompt w else .fail (!c.contains w)
    | .legacy _, .confirmed _ => .prompt w
    | .config c, .rejected | .legacy c, .rejected => .fail (!c.contains w)

/-- Forget the annotation: the part of an outcome the user's turn depends on. -/
def Outcome.erase : Outcome → Outcome
  | .fail _ => .fail false
  | o => o

/-- Every advertised selector sends the requested ID, listed or not. -/
theorem request_always_sent (c : List String) (w : String) :
    sends (.config c) (some w) = true ∧ sends (.legacy c) (some w) = true :=
  ⟨rfl, rfl⟩

/-- The catalog never changes whether a turn proceeds or with which model. -/
theorem catalog_irrelevant_config (c c' : List String) (w : Option String) (r : Reply) :
    (select (.config c) w r).erase = (select (.config c') w r).erase := by
  cases w <;> cases r <;> simp only [select] <;> first | rfl | (split <;> rfl)

theorem catalog_irrelevant_legacy (c c' : List String) (w : Option String) (r : Reply) :
    (select (.legacy c) w r).erase = (select (.legacy c') w r).erase := by
  cases w <;> cases r <;> rfl

/-- An unlisted model the agent confirms is prompted with. -/
theorem unlisted_confirmed_prompts (c : List String) (w : String) :
    select (.config c) (some w) (.confirmed w) = .prompt w ∧
      select (.legacy c) (some w) (.confirmed w) = .prompt w := by
  simp [select]

/-- A prompt only ever uses the requested model. -/
theorem prompt_is_requested (s : Selector) (w : Option String) (r : Reply) (m : String)
    (h : select s w r = .prompt m) : w = some m := by
  cases w with
  | none => simp [select] at h
  | some w =>
    cases s <;> cases r <;> simp only [select] at h
    all_goals first
      | (split at h <;> simp_all)
      | simp_all

/-- A session-config selection prompts only after the agent confirms that value. -/
theorem config_requires_confirmation (c : List String) (w m : String) (r : Reply)
    (h : select (.config c) (some w) r = .prompt m) : r = .confirmed w ∧ m = w := by
  cases r with
  | rejected => simp [select] at h
  | confirmed v =>
    simp only [select] at h
    split at h <;> simp_all

/-- Rejection always fails the turn before any prompt. -/
theorem rejected_fails (s : Selector) (w : String) :
    ∃ a, select s (some w) .rejected = .fail a := by
  cases s <;> exact ⟨_, rfl⟩

/-- Without a usable selector an explicit model fails; nothing is guessed. -/
theorem unusable_selector_fails (w : String) (r : Reply) :
    select .absent (some w) r = .fail false ∧ select .malformed (some w) r = .fail false := by
  cases r <;> simp [select]

/-- Failures carry the catalog note exactly when an unlisted model was sent. -/
theorem annotated_iff_unlisted (s : Selector) (w : String) (r : Reply) (a : Bool)
    (h : select s (some w) r = .fail a) : a = (sends s (some w) && !listed s w) := by
  cases s <;> cases r <;> simp only [select, listed, sends] at h ⊢
  all_goals first
    | (split at h <;> simp_all)
    | simp_all

/-- Without an explicit request the agent's default stays untouched. -/
theorem no_request_keeps_default (s : Selector) (r : Reply) :
    select s none r = .keepDefault ∧ sends s none = false := by
  cases s <;> simp [select, sends]

end Tny.Acp.Model
