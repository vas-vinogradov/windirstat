#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LifecycleFailure {
    pub path: Vec<u16>,
    pub message: Vec<u16>,
    pub error_code: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TerminalReason {
    UserCancel,
    Restarted,
    EngineInterrupted,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TerminalKind {
    Completed,
    Canceled,
    Failed,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TerminalDecision {
    pub kind: TerminalKind,
    pub cancel_reason: TerminalReason,
    pub failure: LifecycleFailure,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LifecycleSnapshot {
    pub outstanding_work: u64,
    pub input_closed: bool,
    pub cancel_requested: bool,
    pub terminal_reached: bool,
    pub terminal_decision_taken: bool,
    pub cancel_reason: TerminalReason,
    pub terminal_decision: Option<TerminalDecision>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LifecycleTransitionResult {
    pub accepted: bool,
    pub ignored_as_late: bool,
    pub terminal_decision: Option<TerminalDecision>,
    pub snapshot: LifecycleSnapshot,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LifecycleTransition {
    Reset,
    Start { work_count: u64 },
    AddOutstandingWork { work_count: u64 },
    FinishOutstandingWork,
    CloseInput,
    RequestCancel { reason: TerminalReason },
    RecordFailure { failure: LifecycleFailure },
}

pub struct LifecycleState {
    outstanding_work: u64,
    input_closed: bool,
    cancel_requested: bool,
    terminal_reached: bool,
    terminal_decision_taken: bool,
    cancel_reason: TerminalReason,
    terminal_decision: Option<TerminalDecision>,
}

impl LifecycleState {
    pub fn new() -> Self {
        Self {
            outstanding_work: 0,
            input_closed: false,
            cancel_requested: false,
            terminal_reached: false,
            terminal_decision_taken: false,
            cancel_reason: TerminalReason::EngineInterrupted,
            terminal_decision: None,
        }
    }

    pub fn apply_transition(
        &mut self,
        transition: LifecycleTransition,
    ) -> LifecycleTransitionResult {
        let is_reset = matches!(transition, LifecycleTransition::Reset);
        let is_start = matches!(transition, LifecycleTransition::Start { .. });
        if self.terminal_reached && !is_reset && !is_start {
            return LifecycleTransitionResult {
                accepted: false,
                ignored_as_late: true,
                terminal_decision: None,
                snapshot: self.snapshot(),
            };
        }

        match transition {
            LifecycleTransition::Reset => self.reset(),
            LifecycleTransition::Start { work_count } => {
                self.reset();
                self.outstanding_work = work_count;
            }
            LifecycleTransition::AddOutstandingWork { work_count } => {
                self.outstanding_work += work_count;
            }
            LifecycleTransition::FinishOutstandingWork => {
                if self.outstanding_work != 0 {
                    self.outstanding_work -= 1;
                }
                self.try_reach_completed();
            }
            LifecycleTransition::CloseInput => {
                self.input_closed = true;
                self.try_reach_completed();
            }
            LifecycleTransition::RequestCancel { reason } => {
                self.cancel_requested = true;
                self.input_closed = true;
                self.cancel_reason = reason;
                self.outstanding_work = 0;
                self.reach_terminal(TerminalDecision {
                    kind: TerminalKind::Canceled,
                    cancel_reason: reason,
                    failure: empty_failure(),
                });
            }
            LifecycleTransition::RecordFailure { failure } => {
                self.outstanding_work = 0;
                self.reach_terminal(TerminalDecision {
                    kind: TerminalKind::Failed,
                    cancel_reason: TerminalReason::EngineInterrupted,
                    failure,
                });
            }
        }

        LifecycleTransitionResult {
            accepted: true,
            ignored_as_late: false,
            terminal_decision: self.terminal_decision.clone(),
            snapshot: self.snapshot(),
        }
    }

    pub fn take_terminal_decision(&mut self) -> Option<TerminalDecision> {
        if self.terminal_decision_taken {
            return None;
        }

        let decision = self.terminal_decision.clone()?;
        self.terminal_decision_taken = true;
        Some(decision)
    }

    pub fn snapshot(&self) -> LifecycleSnapshot {
        LifecycleSnapshot {
            outstanding_work: self.outstanding_work,
            input_closed: self.input_closed,
            cancel_requested: self.cancel_requested,
            terminal_reached: self.terminal_reached,
            terminal_decision_taken: self.terminal_decision_taken,
            cancel_reason: self.cancel_reason,
            terminal_decision: self.terminal_decision.clone(),
        }
    }

    fn reset(&mut self) {
        self.outstanding_work = 0;
        self.input_closed = false;
        self.cancel_requested = false;
        self.terminal_reached = false;
        self.terminal_decision_taken = false;
        self.cancel_reason = TerminalReason::EngineInterrupted;
        self.terminal_decision = None;
    }

    fn try_reach_completed(&mut self) {
        if self.terminal_reached || !self.input_closed || self.outstanding_work != 0 {
            return;
        }

        self.reach_terminal(TerminalDecision {
            kind: TerminalKind::Completed,
            cancel_reason: TerminalReason::EngineInterrupted,
            failure: empty_failure(),
        });
    }

    fn reach_terminal(&mut self, decision: TerminalDecision) {
        if self.terminal_reached {
            return;
        }

        self.terminal_reached = true;
        self.terminal_decision = Some(decision);
    }
}

fn empty_failure() -> LifecycleFailure {
    LifecycleFailure {
        path: Vec::new(),
        message: Vec::new(),
        error_code: 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wide(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    fn take_kind(state: &mut LifecycleState) -> Option<TerminalKind> {
        state.take_terminal_decision().map(|decision| decision.kind)
    }

    #[test]
    fn lifecycle_completes_after_work_drains_and_input_closes() {
        let mut state = LifecycleState::new();
        state.apply_transition(LifecycleTransition::Start { work_count: 1 });
        state.apply_transition(LifecycleTransition::AddOutstandingWork { work_count: 2 });
        state.apply_transition(LifecycleTransition::CloseInput);
        assert!(!state.snapshot().terminal_reached);

        state.apply_transition(LifecycleTransition::FinishOutstandingWork);
        state.apply_transition(LifecycleTransition::FinishOutstandingWork);
        let result = state.apply_transition(LifecycleTransition::FinishOutstandingWork);

        assert!(result.snapshot.terminal_reached);
        assert_eq!(result.snapshot.outstanding_work, 0);
        assert_eq!(take_kind(&mut state), Some(TerminalKind::Completed));
    }

    #[test]
    fn lifecycle_cancellation_before_completion_wins() {
        let mut state = LifecycleState::new();
        state.apply_transition(LifecycleTransition::Start { work_count: 1 });
        state.apply_transition(LifecycleTransition::CloseInput);
        let result = state.apply_transition(LifecycleTransition::RequestCancel {
            reason: TerminalReason::UserCancel,
        });

        assert!(result.snapshot.terminal_reached);
        assert!(result.snapshot.cancel_requested);
        let decision = state.take_terminal_decision().expect("terminal decision");
        assert_eq!(decision.kind, TerminalKind::Canceled);
        assert_eq!(decision.cancel_reason, TerminalReason::UserCancel);
    }

    #[test]
    fn lifecycle_failure_rejects_later_cancel_or_complete() {
        let mut state = LifecycleState::new();
        state.apply_transition(LifecycleTransition::Start { work_count: 1 });
        state.apply_transition(LifecycleTransition::RecordFailure {
            failure: LifecycleFailure {
                path: wide("C:\\broken"),
                message: wide("boom"),
                error_code: 7,
            },
        });

        let late_cancel = state.apply_transition(LifecycleTransition::RequestCancel {
            reason: TerminalReason::UserCancel,
        });
        let late_finish = state.apply_transition(LifecycleTransition::FinishOutstandingWork);

        assert!(!late_cancel.accepted);
        assert!(late_cancel.ignored_as_late);
        assert!(!late_finish.accepted);
        assert_eq!(take_kind(&mut state), Some(TerminalKind::Failed));
    }

    #[test]
    fn lifecycle_rejects_late_transition_after_terminal() {
        let mut state = LifecycleState::new();
        state.apply_transition(LifecycleTransition::Start { work_count: 1 });
        state.apply_transition(LifecycleTransition::CloseInput);
        state.apply_transition(LifecycleTransition::FinishOutstandingWork);

        let late_add =
            state.apply_transition(LifecycleTransition::AddOutstandingWork { work_count: 1 });

        assert!(!late_add.accepted);
        assert!(late_add.ignored_as_late);
        assert_eq!(late_add.snapshot.outstanding_work, 0);
        assert_eq!(take_kind(&mut state), Some(TerminalKind::Completed));
    }

    #[test]
    fn lifecycle_terminal_decision_is_produced_once() {
        let mut state = LifecycleState::new();
        state.apply_transition(LifecycleTransition::Start { work_count: 1 });
        state.apply_transition(LifecycleTransition::CloseInput);
        state.apply_transition(LifecycleTransition::FinishOutstandingWork);

        assert_eq!(take_kind(&mut state), Some(TerminalKind::Completed));
        assert_eq!(take_kind(&mut state), None);
        assert!(state.snapshot().terminal_decision_taken);
    }
}
