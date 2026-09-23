//! One turn's visible state: the status line, the note under the user's
//! message and the note under tny's reply. Every label comes from this machine.
//!
//! Mirrors `proofs/Proofs/Turn.lean` (`stepCur`, `outcome`, the label
//! functions). The Lean file proves that "Sending…" is shown only until tny has
//! the prompt and that a finished process always settles every note; the tests
//! below replay the Lean-generated `proofs/golden/turn*.tsv` against this code.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Idle,
    Sending,
    Waiting,
    Thinking,
    Writing,
    Tool,
    Approval,
    Saving,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UserStatus {
    None,
    Sending,
    Sent,
    Saved,
    Unconfirmed,
    Discarded,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReplyStatus {
    Absent,
    Streaming,
    Done,
    Stopped,
    Incomplete,
}

/// The `ask --events=jsonl` event types the GUI distinguishes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CliKind {
    Thinking,
    Text,
    ToolStart,
    ToolEnd,
    Approval,
    Info,
    Error,
    TurnEndOk,
    TurnEndFail,
}

impl CliKind {
    pub fn of_event(kind: &str, stop_reason: Option<i64>) -> Self {
        match kind {
            "thinking" | "plan" => Self::Thinking,
            "text_delta" => Self::Text,
            "tool_start" => Self::ToolStart,
            "tool_end" => Self::ToolEnd,
            "permission_request" => Self::Approval,
            "error" => Self::Error,
            "turn_end" if stop_reason == Some(0) => Self::TurnEndOk,
            "turn_end" => Self::TurnEndFail,
            _ => Self::Info,
        }
    }
}

/// How a finished turn was reconciled with its saved session.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    Saved,
    SavedFailed,
    Unconfirmed,
    Discarded,
}

/// `confirmed`: the saved session was read back; `accepted`: it holds the new
/// user message; `failed`: the stream or process failed; `had_id`: the stream
/// named a session.
pub fn outcome(confirmed: bool, accepted: bool, failed: bool, had_id: bool) -> Outcome {
    match (confirmed, accepted, failed) {
        (true, true, true) => Outcome::SavedFailed,
        (true, true, false) => Outcome::Saved,
        (true, false, true) => Outcome::Discarded,
        (true, false, false) => Outcome::Unconfirmed,
        (false, _, true) if !had_id => Outcome::Discarded,
        (false, _, _) => Outcome::Unconfirmed,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    Submit,
    Delivered,
    Cli(CliKind),
    Finish(Outcome),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TurnMachine {
    pub phase: Phase,
    pub user: UserStatus,
    pub reply: ReplyStatus,
    pub failed: bool,
}

impl Default for TurnMachine {
    fn default() -> Self {
        Self {
            phase: Phase::Idle,
            user: UserStatus::None,
            reply: ReplyStatus::Absent,
            failed: false,
        }
    }
}

impl TurnMachine {
    pub fn busy(&self) -> bool {
        self.phase != Phase::Idle
    }

    fn live(&self) -> bool {
        !matches!(self.phase, Phase::Idle | Phase::Saving)
    }

    pub fn step(self, event: Event) -> Self {
        match event {
            Event::Submit if self.phase == Phase::Idle => Self {
                phase: Phase::Sending,
                user: UserStatus::Sending,
                reply: ReplyStatus::Absent,
                failed: false,
            },
            Event::Submit => self,
            Event::Delivered if self.phase == Phase::Sending => Self {
                phase: Phase::Waiting,
                user: UserStatus::Sent,
                ..self
            },
            Event::Delivered => self,
            Event::Cli(kind) if self.live() => Self {
                phase: match kind {
                    CliKind::Thinking => Phase::Thinking,
                    CliKind::Text => Phase::Writing,
                    CliKind::ToolStart => Phase::Tool,
                    CliKind::ToolEnd => Phase::Waiting,
                    CliKind::Approval => Phase::Approval,
                    CliKind::Info | CliKind::Error if self.phase == Phase::Sending => {
                        Phase::Waiting
                    }
                    CliKind::Info | CliKind::Error => self.phase,
                    CliKind::TurnEndOk | CliKind::TurnEndFail => Phase::Saving,
                },
                // Any event proves tny read the prompt.
                user: if self.user == UserStatus::Sending {
                    UserStatus::Sent
                } else {
                    self.user
                },
                reply: match (kind, self.reply) {
                    (CliKind::Text, ReplyStatus::Absent) => ReplyStatus::Streaming,
                    (CliKind::TurnEndOk, ReplyStatus::Streaming) => ReplyStatus::Done,
                    (CliKind::TurnEndFail, ReplyStatus::Streaming) => ReplyStatus::Stopped,
                    (_, reply) => reply,
                },
                failed: self.failed || matches!(kind, CliKind::Error | CliKind::TurnEndFail),
            },
            Event::Cli(_) => self,
            Event::Finish(_) if self.phase == Phase::Idle => self,
            Event::Finish(outcome) => Self {
                phase: Phase::Idle,
                user: match outcome {
                    Outcome::Saved | Outcome::SavedFailed => UserStatus::Saved,
                    Outcome::Unconfirmed => UserStatus::Unconfirmed,
                    Outcome::Discarded => UserStatus::Discarded,
                },
                // A stream that ended without `turn_end` is never left streaming.
                reply: if self.reply == ReplyStatus::Streaming {
                    ReplyStatus::Incomplete
                } else {
                    self.reply
                },
                failed: self.failed || matches!(outcome, Outcome::SavedFailed | Outcome::Discarded),
            },
        }
    }

    pub fn user_meta(&self) -> &'static str {
        user_meta(self.user)
    }

    pub fn reply_meta(&self) -> &'static str {
        reply_meta(self.reply)
    }

    /// The status line while a turn is live; `None` when idle, where the
    /// status line shows how the last action ended instead.
    pub fn phase_label(&self) -> Option<&'static str> {
        let label = phase_label(self.phase);
        (!label.is_empty()).then_some(label)
    }
}

pub fn user_meta(status: UserStatus) -> &'static str {
    match status {
        UserStatus::Sending => "Sending…",
        UserStatus::Sent => "Sent",
        UserStatus::Unconfirmed => "Unconfirmed · inspect saved session",
        UserStatus::None | UserStatus::Saved | UserStatus::Discarded => "",
    }
}

pub fn reply_meta(status: ReplyStatus) -> &'static str {
    match status {
        ReplyStatus::Streaming => "Streaming…",
        ReplyStatus::Stopped => "Stopped before finishing",
        ReplyStatus::Incomplete => "Incomplete · the stream ended early",
        ReplyStatus::Absent | ReplyStatus::Done => "",
    }
}

pub fn phase_label(phase: Phase) -> &'static str {
    match phase {
        Phase::Idle => "",
        Phase::Sending => "Sending to tny…",
        Phase::Waiting => "Waiting for the model…",
        Phase::Thinking => "Thinking…",
        Phase::Writing => "Writing…",
        Phase::Tool => "Running a tool…",
        Phase::Approval => "Approval requested · unattended CLI policy applies",
        Phase::Saving => "Saving…",
    }
}

/// `12s`, `3m 04s`: how long the live turn has run.
pub fn elapsed_label(seconds: u64) -> String {
    if seconds < 60 {
        format!("{seconds}s")
    } else {
        format!("{}m {:02}s", seconds / 60, seconds % 60)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PHASES: [(Phase, &str); 8] = [
        (Phase::Idle, "idle"),
        (Phase::Sending, "sending"),
        (Phase::Waiting, "waiting"),
        (Phase::Thinking, "thinking"),
        (Phase::Writing, "writing"),
        (Phase::Tool, "tool"),
        (Phase::Approval, "approval"),
        (Phase::Saving, "saving"),
    ];
    const USERS: [(UserStatus, &str); 6] = [
        (UserStatus::None, "none"),
        (UserStatus::Sending, "sending"),
        (UserStatus::Sent, "sent"),
        (UserStatus::Saved, "saved"),
        (UserStatus::Unconfirmed, "unconfirmed"),
        (UserStatus::Discarded, "discarded"),
    ];
    const REPLIES: [(ReplyStatus, &str); 5] = [
        (ReplyStatus::Absent, "absent"),
        (ReplyStatus::Streaming, "streaming"),
        (ReplyStatus::Done, "done"),
        (ReplyStatus::Stopped, "stopped"),
        (ReplyStatus::Incomplete, "incomplete"),
    ];
    const CLI: [(CliKind, &str); 9] = [
        (CliKind::Thinking, "thinking"),
        (CliKind::Text, "text"),
        (CliKind::ToolStart, "tool_start"),
        (CliKind::ToolEnd, "tool_end"),
        (CliKind::Approval, "approval"),
        (CliKind::Info, "info"),
        (CliKind::Error, "error"),
        (CliKind::TurnEndOk, "turn_end_ok"),
        (CliKind::TurnEndFail, "turn_end_fail"),
    ];
    const OUTCOMES: [(Outcome, &str); 4] = [
        (Outcome::Saved, "saved"),
        (Outcome::SavedFailed, "saved_failed"),
        (Outcome::Unconfirmed, "unconfirmed"),
        (Outcome::Discarded, "discarded"),
    ];

    fn lookup<T: Copy>(table: &[(T, &str)], name: &str) -> T {
        table
            .iter()
            .find(|(_, n)| *n == name)
            .unwrap_or_else(|| panic!("unknown name {name}"))
            .0
    }

    fn event(name: &str) -> Event {
        match name.split_once(':') {
            None if name == "submit" => Event::Submit,
            None if name == "delivered" => Event::Delivered,
            Some(("cli", kind)) => Event::Cli(lookup(&CLI, kind)),
            Some(("finish", outcome)) => Event::Finish(lookup(&OUTCOMES, outcome)),
            _ => panic!("unknown event {name}"),
        }
    }

    fn state(cols: &[&str]) -> TurnMachine {
        TurnMachine {
            phase: lookup(&PHASES, cols[0]),
            user: lookup(&USERS, cols[1]),
            reply: lookup(&REPLIES, cols[2]),
            failed: cols[3] == "1",
        }
    }

    fn rows(table: &str) -> impl Iterator<Item = Vec<&str>> {
        table.lines().skip(1).map(|line| line.split('\t').collect())
    }

    #[test]
    fn step_matches_every_row_of_the_proven_lean_table() {
        let mut count = 0;
        for cols in rows(include_str!("../proofs/golden/turn.tsv")) {
            let before = state(&cols[0..4]);
            let after = state(&cols[5..9]);
            assert_eq!(before.step(event(cols[4])), after, "{cols:?}");
            count += 1;
        }
        // Every state satisfying Turn.Inv × every event.
        assert_eq!(count, 1410);
    }

    #[test]
    fn labels_match_the_lean_label_functions() {
        for cols in rows(include_str!("../proofs/golden/turn_labels.tsv")) {
            let label = match cols[0] {
                "phase" => phase_label(lookup(&PHASES, cols[1])),
                "user" => user_meta(lookup(&USERS, cols[1])),
                "reply" => reply_meta(lookup(&REPLIES, cols[1])),
                other => panic!("unknown label kind {other}"),
            };
            assert_eq!(label, cols.get(2).copied().unwrap_or(""), "{cols:?}");
        }
    }

    #[test]
    fn outcome_matches_the_lean_classification() {
        for cols in rows(include_str!("../proofs/golden/turn_outcome.tsv")) {
            let bit = |i: usize| cols[i] == "1";
            assert_eq!(
                outcome(bit(0), bit(1), bit(2), bit(3)),
                lookup(&OUTCOMES, cols[4]),
                "{cols:?}"
            );
        }
    }

    #[test]
    fn a_long_tool_run_shows_sent_not_sending() {
        // The reported bug: the prompt was delivered, a tool ran for minutes
        // before the first event, and the message still said "Sending…".
        let m = TurnMachine::default().step(Event::Submit);
        assert_eq!(m.user_meta(), "Sending…");
        let m = m.step(Event::Delivered);
        assert_eq!(m.user_meta(), "Sent");
        assert_eq!(m.phase_label(), Some("Waiting for the model…"));
        let m = m.step(Event::Cli(CliKind::ToolStart));
        assert_eq!(m.phase_label(), Some("Running a tool…"));
        let m = m
            .step(Event::Cli(CliKind::ToolEnd))
            .step(Event::Cli(CliKind::Text))
            .step(Event::Cli(CliKind::TurnEndOk))
            .step(Event::Finish(Outcome::Saved));
        assert_eq!(
            (m.user_meta(), m.reply_meta(), m.phase_label()),
            ("", "", None)
        );
    }

    #[test]
    fn a_stream_without_turn_end_never_stays_streaming() {
        let m = TurnMachine::default()
            .step(Event::Submit)
            .step(Event::Cli(CliKind::Text))
            .step(Event::Finish(Outcome::Unconfirmed));
        assert_eq!(m.reply_meta(), "Incomplete · the stream ended early");
        assert_eq!(m.user_meta(), "Unconfirmed · inspect saved session");
        assert!(!m.busy());
    }

    #[test]
    fn cli_event_names_map_to_kinds() {
        assert_eq!(CliKind::of_event("turn_end", Some(0)), CliKind::TurnEndOk);
        assert_eq!(CliKind::of_event("turn_end", Some(4)), CliKind::TurnEndFail);
        assert_eq!(CliKind::of_event("turn_end", None), CliKind::TurnEndFail);
        assert_eq!(CliKind::of_event("usage", None), CliKind::Info);
        assert_eq!(
            CliKind::of_event("permission_request", None),
            CliKind::Approval
        );
        assert_eq!(elapsed_label(7), "7s");
        assert_eq!(elapsed_label(184), "3m 04s");
    }
}
