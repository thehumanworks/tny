; TUI shell-mode transition abstraction (docs/adr/0173).
; Each state has a running child, a completed-but-undelivered disclosure,
; and a prompt submission. A command is permitted only if it fits the
; disclosure budget; a prompt is permitted only after the child exits.
; Linked concrete fixtures: shell_mode_streams_and_discloses_only_once,
; shell_mode_denies_unrecordable_command in tests/test_tui.c.
(set-logic QF_LIA)
(declare-const running Bool)
(declare-const fit Bool)
(declare-const start Bool)
(declare-const submit Bool)
(declare-const pending Int)
(declare-const added Int)
(declare-const next Int)
(declare-const delivered Int)
(declare-const pending-after Int)
(declare-const second-delivered Int)
(assert (>= pending 0))
(assert (>= added 0))
(assert (<= pending 65536))
(assert (= fit (<= (+ pending added 128) 65536)))
(assert (= start (and (not running) fit)))
(assert (=> start (= next (+ pending added))))
(assert (=> (not start) (= next pending)))
(assert (=> submit (not running)))
(assert (= delivered (ite submit next 0)))
(assert (= pending-after (ite submit 0 next)))
(assert (= second-delivered pending-after))
; A command cannot start if another child runs or its disclosure cannot fit.
(push 1)
(assert (and start (or running (not fit))))
(check-sat)
(pop 1)
; Every permitted command adds its record to the pending disclosure.
(push 1)
(assert (and start (not (= next (+ pending added)))))
(check-sat)
(pop 1)
; A pending record cannot be delivered while output is still streaming.
(push 1)
(assert (and running (> delivered 0)))
(check-sat)
(pop 1)
; One successful prompt delivers the full accumulated record.
(push 1)
(assert (and submit (not (= delivered next))))
(check-sat)
(pop 1)
; Successful delivery clears the record, so a second send cannot replay it.
(push 1)
(assert (and submit (> second-delivered 0)))
(check-sat)
(pop 1)
; Neither command records nor output can exceed the context budget.
(push 1)
(assert (and start (> next 65536)))
(check-sat)
(pop 1)
