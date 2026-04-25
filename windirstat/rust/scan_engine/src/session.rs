use crate::discovery::{self, DiscoveredEntry};
use crate::lifecycle::{
    LifecycleFailure, LifecycleSnapshot, LifecycleState, LifecycleTransition, TerminalDecision,
    TerminalReason,
};
use crate::scan::{DiscoveryReportOutcome, FollowPolicy, ScanCoordinator, TraversalChildCandidate};
use std::collections::VecDeque;
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

// Intent: session-level scan orchestration owned by the Rust engine.
// Contract: this type owns traversal progression, immediate-child discovery,
// outstanding work, worker-owned discovery, and terminal lifecycle decisions
// for a single scan request. The host remains an RPC emitter and request-input
// shell.
pub struct ScanSession {
    coordinator: ScanCoordinator,
    lifecycle: LifecycleState,
    completed_discoveries: VecDeque<DiscoveryCompletion>,
    workers: Option<DiscoveryWorkers>,
    worker_count: usize,
    in_flight_work: usize,
    timing: ScanSessionTimingStats,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ScanSessionTimingStats {
    pub discovery_worker_elapsed_ns: u64,
    pub scheduling_elapsed_ns: u64,
    pub no_event_poll_count: u64,
    pub worker_count: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DirectoryDiscoveryAction {
    pub directory_path: Vec<u16>,
    pub discovered_entries: Vec<DiscoveredEntry>,
    pub scheduled_paths: Vec<Vec<u16>>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ScanSessionAction {
    #[allow(dead_code)]
    RequestDirectory(Vec<u16>),
    EmitDirectoryResult(DirectoryDiscoveryAction),
    WaitingForInput,
    Complete,
    Cancelled(TerminalReason),
    Failed(LifecycleFailure),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DirectoryReportOutcome {
    pub scheduled_paths: Vec<Vec<u16>>,
}

struct DiscoveryCompletion {
    directory_path: Vec<u16>,
    result: Result<Vec<DiscoveredEntry>, String>,
    discovery_elapsed: Duration,
}

enum DiscoveryWorkerMessage {
    Discover(Vec<u16>),
    Shutdown,
}

struct DiscoveryWorkers {
    sender: Sender<DiscoveryWorkerMessage>,
    result_receiver: Receiver<DiscoveryCompletion>,
    handles: Vec<JoinHandle<()>>,
}

impl DiscoveryWorkers {
    fn new(worker_count: usize) -> Self {
        let (sender, receiver) = mpsc::channel::<DiscoveryWorkerMessage>();
        let (result_sender, result_receiver) = mpsc::channel::<DiscoveryCompletion>();
        let shared_receiver = std::sync::Arc::new(std::sync::Mutex::new(receiver));
        let mut handles = Vec::with_capacity(worker_count);

        for _ in 0..worker_count {
            let worker_receiver = std::sync::Arc::clone(&shared_receiver);
            let worker_result_sender = result_sender.clone();
            handles.push(thread::spawn(move || loop {
                let message = {
                    let receiver = worker_receiver
                        .lock()
                        .expect("discovery worker receiver mutex poisoned");
                    receiver.recv()
                };

                match message {
                    Ok(DiscoveryWorkerMessage::Discover(directory_path)) => {
                        let started = Instant::now();
                        let result = discovery::discover_directory_entries(&directory_path);
                        let _ = worker_result_sender.send(DiscoveryCompletion {
                            directory_path,
                            result,
                            discovery_elapsed: started.elapsed(),
                        });
                    }
                    Ok(DiscoveryWorkerMessage::Shutdown) | Err(_) => break,
                }
            }));
        }

        Self {
            sender,
            result_receiver,
            handles,
        }
    }

    fn try_recv(&self) -> Result<DiscoveryCompletion, TryRecvError> {
        self.result_receiver.try_recv()
    }

    fn send_discovery(&self, path: Vec<u16>) -> bool {
        self.sender
            .send(DiscoveryWorkerMessage::Discover(path))
            .is_ok()
    }

    fn shutdown(self) {
        for _ in &self.handles {
            let _ = self.sender.send(DiscoveryWorkerMessage::Shutdown);
        }

        for handle in self.handles {
            let _ = handle.join();
        }
    }
}

impl ScanSession {
    pub fn new() -> Self {
        Self::new_with_worker_count(default_worker_count())
    }

    pub fn new_with_worker_count(worker_count: usize) -> Self {
        Self {
            coordinator: ScanCoordinator::new(),
            lifecycle: LifecycleState::new(),
            completed_discoveries: VecDeque::new(),
            workers: None,
            worker_count: worker_count.max(1),
            in_flight_work: 0,
            timing: ScanSessionTimingStats {
                worker_count: worker_count.max(1),
                ..ScanSessionTimingStats::default()
            },
        }
    }

    pub fn start(&mut self, root_path: Vec<u16>, follow_policy: FollowPolicy) {
        self.shutdown_workers();
        self.completed_discoveries.clear();
        self.in_flight_work = 0;
        self.timing = ScanSessionTimingStats {
            worker_count: self.worker_count,
            ..ScanSessionTimingStats::default()
        };
        self.coordinator.start(root_path, follow_policy);
        self.lifecycle
            .apply_transition(LifecycleTransition::Start { work_count: 1 });
    }

    pub fn reset(&mut self) {
        self.shutdown_workers();
        self.completed_discoveries.clear();
        self.in_flight_work = 0;
        self.timing = ScanSessionTimingStats {
            worker_count: self.worker_count,
            ..ScanSessionTimingStats::default()
        };
        self.coordinator.clear();
        self.lifecycle.apply_transition(LifecycleTransition::Reset);
    }

    pub fn close_input(&mut self) {
        self.lifecycle
            .apply_transition(LifecycleTransition::CloseInput);
    }

    pub fn add_external_path(&mut self, path: Vec<u16>) -> bool {
        if self.should_ignore_progression_input() || self.lifecycle.snapshot().input_closed {
            return false;
        }

        if !self.coordinator.add_path(path) {
            return false;
        }

        self.lifecycle
            .apply_transition(LifecycleTransition::AddOutstandingWork { work_count: 1 });
        true
    }

    pub fn clear_pending_work(&mut self) {
        self.coordinator.clear();
    }

    pub fn request_cancel(&mut self, reason: TerminalReason) {
        self.coordinator.clear();
        self.completed_discoveries.clear();
        self.in_flight_work = 0;
        self.shutdown_workers();
        self.lifecycle
            .apply_transition(LifecycleTransition::RequestCancel { reason });
    }

    pub fn next_action(&mut self) -> ScanSessionAction {
        if self.worker_count > 1 {
            return self.next_concurrent_action();
        }

        let snapshot = self.lifecycle.snapshot();
        if let Some(decision) = snapshot
            .terminal_decision
            .filter(|_| !snapshot.terminal_decision_taken)
        {
            return match decision.kind {
                crate::lifecycle::TerminalKind::Completed => ScanSessionAction::Complete,
                crate::lifecycle::TerminalKind::Canceled => {
                    ScanSessionAction::Cancelled(decision.cancel_reason)
                }
                crate::lifecycle::TerminalKind::Failed => {
                    ScanSessionAction::Failed(decision.failure)
                }
            };
        }

        match self.coordinator.take_next() {
            Some(path) => match self.discover_directory_and_update_session(&path) {
                Ok(action) => action,
                Err(failure) => {
                    self.report_directory_failure(failure.clone());
                    ScanSessionAction::Failed(failure)
                }
            },
            None => {
                self.timing.no_event_poll_count = self.timing.no_event_poll_count.saturating_add(1);
                ScanSessionAction::WaitingForInput
            }
        }
    }

    pub fn timing_stats(&self) -> ScanSessionTimingStats {
        self.timing
    }

    pub fn take_next_path(&mut self) -> Option<Vec<u16>> {
        self.coordinator.take_next()
    }

    pub fn report_directory_result(
        &mut self,
        child_candidates: &[TraversalChildCandidate],
    ) -> DirectoryReportOutcome {
        if self.should_ignore_progression_input() {
            return DirectoryReportOutcome {
                scheduled_paths: Vec::new(),
            };
        }

        let scheduling_started = Instant::now();
        let DiscoveryReportOutcome { scheduled_paths } =
            self.coordinator.report_discovery_result(child_candidates);
        let scheduled_count = scheduled_paths.len() as u64;
        if scheduled_count != 0 {
            self.lifecycle
                .apply_transition(LifecycleTransition::AddOutstandingWork {
                    work_count: scheduled_count,
                });
        }
        self.lifecycle
            .apply_transition(LifecycleTransition::FinishOutstandingWork);
        self.timing.scheduling_elapsed_ns = self
            .timing
            .scheduling_elapsed_ns
            .saturating_add(nanos_u64(scheduling_started.elapsed()));

        DirectoryReportOutcome { scheduled_paths }
    }

    pub fn report_directory_failure(&mut self, failure: LifecycleFailure) {
        if self.should_ignore_progression_input() {
            return;
        }

        self.coordinator.clear();
        self.completed_discoveries.clear();
        self.in_flight_work = 0;
        self.shutdown_workers();
        self.lifecycle
            .apply_transition(LifecycleTransition::RecordFailure { failure });
    }

    pub fn has_terminal_decision(&self) -> bool {
        let snapshot = self.lifecycle.snapshot();
        snapshot.terminal_decision.is_some() && !snapshot.terminal_decision_taken
    }

    pub fn take_terminal_decision(&mut self) -> Option<TerminalDecision> {
        self.lifecycle.take_terminal_decision()
    }

    pub fn pending_work_count(&self) -> usize {
        self.coordinator.pending_count()
    }

    pub fn is_done(&self) -> bool {
        self.coordinator.is_done()
            && self.in_flight_work == 0
            && self.completed_discoveries.is_empty()
    }

    pub fn lifecycle_snapshot(&self) -> LifecycleSnapshot {
        self.lifecycle.snapshot()
    }

    fn should_ignore_progression_input(&self) -> bool {
        let snapshot = self.lifecycle.snapshot();
        snapshot.terminal_reached || snapshot.cancel_requested
    }

    fn next_concurrent_action(&mut self) -> ScanSessionAction {
        self.drain_worker_completions();

        let snapshot = self.lifecycle.snapshot();
        if let Some(decision) = snapshot
            .terminal_decision
            .filter(|_| !snapshot.terminal_decision_taken)
        {
            self.shutdown_workers();
            return match decision.kind {
                crate::lifecycle::TerminalKind::Completed => ScanSessionAction::Complete,
                crate::lifecycle::TerminalKind::Canceled => {
                    ScanSessionAction::Cancelled(decision.cancel_reason)
                }
                crate::lifecycle::TerminalKind::Failed => {
                    ScanSessionAction::Failed(decision.failure)
                }
            };
        }

        if let Some(completion) = self.completed_discoveries.pop_front() {
            return self.apply_discovery_completion(completion);
        }

        self.fill_worker_queue();
        self.drain_worker_completions();

        if let Some(completion) = self.completed_discoveries.pop_front() {
            return self.apply_discovery_completion(completion);
        }

        self.timing.no_event_poll_count = self.timing.no_event_poll_count.saturating_add(1);
        ScanSessionAction::WaitingForInput
    }

    fn fill_worker_queue(&mut self) {
        if self.should_ignore_progression_input() {
            return;
        }

        while self.in_flight_work < self.worker_count {
            let Some(path) = self.coordinator.take_next() else {
                break;
            };

            let workers = self
                .workers
                .get_or_insert_with(|| DiscoveryWorkers::new(self.worker_count));
            if workers.send_discovery(path) {
                self.in_flight_work += 1;
            } else {
                self.report_directory_failure(LifecycleFailure {
                    path: Vec::new(),
                    message: "discovery worker channel closed".encode_utf16().collect(),
                    error_code: 0,
                });
                break;
            }
        }
    }

    fn drain_worker_completions(&mut self) {
        let Some(workers) = self.workers.as_ref() else {
            return;
        };

        loop {
            match workers.try_recv() {
                Ok(completion) => self.completed_discoveries.push_back(completion),
                Err(TryRecvError::Empty) | Err(TryRecvError::Disconnected) => break,
            }
        }
    }

    fn apply_discovery_completion(&mut self, completion: DiscoveryCompletion) -> ScanSessionAction {
        self.in_flight_work = self.in_flight_work.saturating_sub(1);
        self.timing.discovery_worker_elapsed_ns = self
            .timing
            .discovery_worker_elapsed_ns
            .saturating_add(nanos_u64(completion.discovery_elapsed));

        match completion.result {
            Ok(discovered_entries) => {
                let action =
                    self.apply_discovered_entries(completion.directory_path, discovered_entries);
                self.fill_worker_queue();
                action
            }
            Err(message) => {
                let failure = LifecycleFailure {
                    path: completion.directory_path,
                    message: message.encode_utf16().collect(),
                    error_code: 0,
                };
                self.report_directory_failure(failure.clone());
                ScanSessionAction::Failed(failure)
            }
        }
    }

    fn shutdown_workers(&mut self) {
        if let Some(workers) = self.workers.take() {
            workers.shutdown();
        }
    }

    fn discover_directory_and_update_session(
        &mut self,
        directory_path: &[u16],
    ) -> Result<ScanSessionAction, LifecycleFailure> {
        let discovery_started = Instant::now();
        let discovered_entries =
            discovery::discover_directory_entries(directory_path).map_err(|message| {
                LifecycleFailure {
                    path: directory_path.to_vec(),
                    message: message.encode_utf16().collect(),
                    error_code: 0,
                }
            })?;
        self.timing.discovery_worker_elapsed_ns = self
            .timing
            .discovery_worker_elapsed_ns
            .saturating_add(nanos_u64(discovery_started.elapsed()));

        Ok(self.apply_discovered_entries(directory_path.to_vec(), discovered_entries))
    }

    fn apply_discovered_entries(
        &mut self,
        directory_path: Vec<u16>,
        discovered_entries: Vec<DiscoveredEntry>,
    ) -> ScanSessionAction {
        let scheduling_started = Instant::now();
        let child_candidates: Vec<TraversalChildCandidate> = discovered_entries
            .iter()
            .filter_map(|entry| match entry.kind {
                discovery::DiscoveredEntryKind::Directory => Some(TraversalChildCandidate {
                    full_path: entry.full_path.clone(),
                    reparse_tag: entry.reparse_tag,
                    is_protected_reparse_point: entry.is_protected_reparse_point,
                }),
                discovery::DiscoveredEntryKind::File => None,
            })
            .collect();
        let DiscoveryReportOutcome { scheduled_paths } =
            self.coordinator.report_discovery_result(&child_candidates);
        let scheduled_count = scheduled_paths.len() as u64;
        if scheduled_count != 0 {
            self.lifecycle
                .apply_transition(LifecycleTransition::AddOutstandingWork {
                    work_count: scheduled_count,
                });
        }
        self.lifecycle
            .apply_transition(LifecycleTransition::FinishOutstandingWork);
        self.timing.scheduling_elapsed_ns = self
            .timing
            .scheduling_elapsed_ns
            .saturating_add(nanos_u64(scheduling_started.elapsed()));

        ScanSessionAction::EmitDirectoryResult(DirectoryDiscoveryAction {
            directory_path,
            discovered_entries,
            scheduled_paths,
        })
    }
}

fn nanos_u64(duration: Duration) -> u64 {
    duration.as_nanos().min(u128::from(u64::MAX)) as u64
}

impl Drop for ScanSession {
    fn drop(&mut self) {
        self.shutdown_workers();
    }
}

fn default_worker_count() -> usize {
    if let Ok(value) = std::env::var("WINDIRSTAT_RUST_SCAN_THREADS") {
        if let Ok(parsed) = value.parse::<usize>() {
            return parsed.clamp(1, 16);
        }
    }

    #[cfg(test)]
    {
        1
    }

    #[cfg(not(test))]
    {
        thread::available_parallelism()
            .map(|count| count.get())
            .unwrap_or(4)
            .clamp(1, 8)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::discovery;
    use crate::lifecycle::{TerminalKind, TerminalReason};
    use crate::scan::IO_REPARSE_TAG_SYMLINK_VALUE;
    use std::fs;
    use std::path::PathBuf;
    use std::rc::Rc;
    use std::thread;
    use std::time::Duration;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn wide(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    fn policy() -> FollowPolicy {
        FollowPolicy {
            follow_mount_points: false,
            follow_symbolic_links: false,
            follow_junctions: false,
        }
    }

    fn candidate(full_path: &[u16]) -> TraversalChildCandidate {
        TraversalChildCandidate {
            full_path: full_path.to_vec(),
            reparse_tag: 0,
            is_protected_reparse_point: false,
        }
    }

    fn take_kind(session: &mut ScanSession) -> Option<TerminalKind> {
        session
            .take_terminal_decision()
            .map(|decision| decision.kind)
    }

    fn unique_temp_dir(test_name: &str) -> PathBuf {
        let ticks = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock before epoch")
            .as_nanos();
        std::env::temp_dir().join(format!("windirstat-scan-session-{test_name}-{ticks}"))
    }

    fn path_to_wide(path: &std::path::Path) -> Vec<u16> {
        path.as_os_str().to_string_lossy().encode_utf16().collect()
    }

    fn collect_scan_actions(session: &mut ScanSession) -> Vec<DirectoryDiscoveryAction> {
        let mut results = Vec::new();
        for _ in 0..1_000 {
            match session.next_action() {
                ScanSessionAction::EmitDirectoryResult(batch) => results.push(batch),
                ScanSessionAction::Complete => return results,
                ScanSessionAction::WaitingForInput => thread::sleep(Duration::from_millis(1)),
                ScanSessionAction::Failed(failure) => {
                    panic!(
                        "scan failed: {}",
                        String::from_utf16_lossy(&failure.message)
                    );
                }
                ScanSessionAction::Cancelled(reason) => panic!("scan cancelled: {reason:?}"),
                ScanSessionAction::RequestDirectory(_) => panic!("unexpected request action"),
            }
        }

        panic!("scan did not complete");
    }

    #[test]
    fn session_progresses_multi_step_scan_to_completion() {
        let root = wide("C:\\root");
        let first = wide("C:\\root\\first");
        let second = wide("C:\\root\\second");
        let grandchild = wide("C:\\root\\first\\leaf");
        let mut session = ScanSession::new();

        session.start(root.clone(), policy());
        assert_eq!(session.take_next_path(), Some(root));

        let first_report =
            session.report_directory_result(&[candidate(&first), candidate(&second)]);
        assert_eq!(
            first_report.scheduled_paths,
            vec![first.clone(), second.clone()]
        );
        assert_eq!(session.lifecycle_snapshot().outstanding_work, 2);

        assert_eq!(session.take_next_path(), Some(first.clone()));
        let second_report = session.report_directory_result(&[candidate(&grandchild)]);
        assert_eq!(second_report.scheduled_paths, vec![grandchild.clone()]);

        assert_eq!(session.take_next_path(), Some(second));
        assert!(session
            .report_directory_result(&[])
            .scheduled_paths
            .is_empty());

        session.close_input();
        assert!(!session.has_terminal_decision());

        assert_eq!(session.take_next_path(), Some(grandchild));
        assert!(session
            .report_directory_result(&[])
            .scheduled_paths
            .is_empty());
        assert!(session.has_terminal_decision());
        assert_eq!(session.next_action(), ScanSessionAction::Complete);
        assert_eq!(take_kind(&mut session), Some(TerminalKind::Completed));
    }

    #[test]
    fn session_empty_result_completes_after_input_closes() {
        let root = wide("C:\\empty");
        let mut session = ScanSession::new();

        session.start(root.clone(), policy());
        assert_eq!(session.take_next_path(), Some(root));
        assert!(session
            .report_directory_result(&[])
            .scheduled_paths
            .is_empty());
        assert!(!session.has_terminal_decision());

        session.close_input();

        assert!(session.has_terminal_decision());
        assert_eq!(take_kind(&mut session), Some(TerminalKind::Completed));
    }

    #[test]
    fn session_failure_clears_pending_work_and_takes_failed_terminal() {
        let root = wide("C:\\root");
        let child = wide("C:\\root\\child");
        let mut session = ScanSession::new();

        session.start(root.clone(), policy());
        assert_eq!(session.take_next_path(), Some(root));
        assert_eq!(
            session
                .report_directory_result(&[candidate(&child)])
                .scheduled_paths,
            vec![child.clone()]
        );
        assert_eq!(session.pending_work_count(), 1);

        session.report_directory_failure(LifecycleFailure {
            path: child,
            message: wide("boom"),
            error_code: 7,
        });

        assert_eq!(session.pending_work_count(), 0);
        let decision = session
            .take_terminal_decision()
            .expect("failed terminal decision");
        assert_eq!(decision.kind, TerminalKind::Failed);
        assert_eq!(decision.failure.error_code, 7);
    }

    #[test]
    fn session_cancellation_clears_queue_and_ignores_later_results() {
        let root = wide("C:\\root");
        let child = wide("C:\\root\\child");
        let mut session = ScanSession::new();

        session.start(root.clone(), policy());
        assert_eq!(session.take_next_path(), Some(root));
        assert_eq!(
            session
                .report_directory_result(&[candidate(&child)])
                .scheduled_paths,
            vec![child.clone()]
        );
        assert_eq!(session.pending_work_count(), 1);

        session.request_cancel(TerminalReason::UserCancel);

        assert_eq!(session.pending_work_count(), 0);
        assert!(session.lifecycle_snapshot().cancel_requested);
        assert!(session.lifecycle_snapshot().input_closed);
        assert!(session
            .report_directory_result(&[candidate(&wide("C:\\late"))])
            .scheduled_paths
            .is_empty());
        assert_eq!(
            session.next_action(),
            ScanSessionAction::Cancelled(TerminalReason::UserCancel)
        );
        let decision = session
            .take_terminal_decision()
            .expect("canceled terminal decision");
        assert_eq!(decision.kind, TerminalKind::Canceled);
        assert_eq!(decision.cancel_reason, TerminalReason::UserCancel);
    }

    #[test]
    fn session_terminal_decision_availability_tracks_exactly_once_take() {
        let root = wide("C:\\root");
        let mut session = ScanSession::new();

        session.start(root.clone(), policy());
        assert_eq!(session.take_next_path(), Some(root));
        session.report_directory_result(&[]);
        session.close_input();

        assert!(session.has_terminal_decision());
        assert_eq!(take_kind(&mut session), Some(TerminalKind::Completed));
        assert!(!session.has_terminal_decision());
        assert_eq!(take_kind(&mut session), None);
    }

    #[test]
    fn session_preserves_traversal_policy_inside_result_reporting() {
        let root = wide("C:\\root");
        let link = wide("C:\\root\\link");
        let mut session = ScanSession::new();

        session.start(root.clone(), policy());
        assert_eq!(session.take_next_path(), Some(root));
        let report = session.report_directory_result(&[TraversalChildCandidate {
            full_path: link,
            reparse_tag: IO_REPARSE_TAG_SYMLINK_VALUE,
            is_protected_reparse_point: false,
        }]);

        assert!(report.scheduled_paths.is_empty());
        assert_eq!(session.pending_work_count(), 0);
    }

    #[test]
    fn session_waits_for_input_after_queue_drains_without_terminal() {
        let root = wide("C:\\root");
        let mut session = ScanSession::new();

        session.start(root.clone(), policy());
        assert_eq!(session.take_next_path(), Some(root));
        session.report_directory_result(&[]);

        assert_eq!(session.next_action(), ScanSessionAction::WaitingForInput);
    }

    #[test]
    fn session_reports_failed_action_after_directory_failure() {
        let root = wide("C:\\root");
        let failure = LifecycleFailure {
            path: wide("C:\\root\\broken"),
            message: wide("boom"),
            error_code: 99,
        };
        let mut session = ScanSession::new();

        session.start(root.clone(), policy());
        assert_eq!(session.take_next_path(), Some(root));
        session.report_directory_failure(failure.clone());

        assert_eq!(session.next_action(), ScanSessionAction::Failed(failure));
    }

    #[test]
    fn next_action_discovers_directory_and_returns_emit_ready_batch() {
        let root = unique_temp_dir("emit-batch");
        fs::create_dir_all(root.join("child")).expect("create child directory");
        fs::write(root.join("alpha.txt"), b"alpha").expect("create file");
        let root_wide: Vec<u16> = root.as_os_str().to_string_lossy().encode_utf16().collect();
        let mut session = ScanSession::new();

        session.start(root_wide.clone(), policy());
        let action = session.next_action();

        let ScanSessionAction::EmitDirectoryResult(batch) = action else {
            panic!("expected emit-ready directory result");
        };
        assert_eq!(batch.directory_path, root_wide);
        assert_eq!(batch.discovered_entries.len(), 2);
        assert_eq!(batch.scheduled_paths.len(), 1);
        assert_eq!(session.pending_work_count(), 1);

        fs::remove_dir_all(root).expect("remove temp tree");
    }

    #[test]
    fn next_action_reports_failed_terminal_when_discovery_fails() {
        let broken = wide("C:\\broken");
        let mut session = ScanSession::new();

        discovery::set_test_discovery_override(Some(Rc::new(|_| Err("boom".to_string()))));
        session.start(broken.clone(), policy());
        let action = session.next_action();
        discovery::set_test_discovery_override(None);

        let ScanSessionAction::Failed(failure) = action else {
            panic!("expected failed action");
        };
        assert_eq!(failure.path, broken);
        assert_eq!(failure.message, wide("boom"));
        assert!(session.has_terminal_decision());
        assert_eq!(take_kind(&mut session), Some(TerminalKind::Failed));
    }

    #[test]
    fn multi_worker_scan_emits_all_directories_once() {
        let root = unique_temp_dir("multi-worker-all-dirs");
        fs::create_dir_all(root.join("a").join("leaf")).expect("create a leaf");
        fs::create_dir_all(root.join("b")).expect("create b");
        fs::write(root.join("root.txt"), b"root").expect("create root file");
        fs::write(root.join("a").join("a.txt"), b"alpha").expect("create a file");
        fs::write(root.join("a").join("leaf").join("leaf.txt"), b"leaf").expect("create leaf file");
        fs::write(root.join("b").join("b.txt"), b"beta").expect("create b file");

        let mut session = ScanSession::new_with_worker_count(4);
        session.start(path_to_wide(&root), policy());
        session.close_input();

        let batches = collect_scan_actions(&mut session);
        let mut paths: Vec<String> = batches
            .iter()
            .map(|batch| String::from_utf16_lossy(&batch.directory_path))
            .collect();
        paths.sort();
        paths.dedup();

        assert_eq!(paths.len(), 4);
        assert_eq!(batches.len(), 4);
        assert!(session.is_done());

        fs::remove_dir_all(root).expect("remove temp tree");
    }

    #[test]
    fn multi_worker_completion_waits_for_in_flight_result_drain() {
        let root = unique_temp_dir("multi-worker-completion");
        fs::create_dir_all(root.join("child")).expect("create child");
        fs::write(root.join("child").join("child.txt"), b"child").expect("create child file");

        let mut session = ScanSession::new_with_worker_count(2);
        session.start(path_to_wide(&root), policy());
        session.close_input();

        assert_eq!(session.next_action(), ScanSessionAction::WaitingForInput);
        assert!(!session.has_terminal_decision());

        let batches = collect_scan_actions(&mut session);
        assert_eq!(batches.len(), 2);
        assert!(session.has_terminal_decision());

        fs::remove_dir_all(root).expect("remove temp tree");
    }

    #[test]
    fn multi_worker_generated_fixture_logical_total_is_stable() {
        let root = unique_temp_dir("multi-worker-logical-total");
        fs::create_dir_all(root.join("left")).expect("create left");
        fs::create_dir_all(root.join("right")).expect("create right");
        fs::write(root.join("left").join("one.bin"), vec![1_u8; 11]).expect("create one");
        fs::write(root.join("right").join("two.bin"), vec![2_u8; 17]).expect("create two");
        fs::write(root.join("three.bin"), vec![3_u8; 23]).expect("create three");

        let mut session = ScanSession::new_with_worker_count(4);
        session.start(path_to_wide(&root), policy());
        session.close_input();

        let logical_total: u64 = collect_scan_actions(&mut session)
            .iter()
            .flat_map(|batch| &batch.discovered_entries)
            .filter(|entry| entry.kind == discovery::DiscoveredEntryKind::File)
            .map(|entry| entry.size_logical)
            .sum();

        assert_eq!(logical_total, 51);

        fs::remove_dir_all(root).expect("remove temp tree");
    }
}
