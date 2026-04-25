use std::collections::{HashSet, VecDeque};

pub const IO_REPARSE_TAG_MOUNT_POINT_VALUE: u32 = 0xA0000003;
pub const IO_REPARSE_TAG_SYMLINK_VALUE: u32 = 0xA000000C;
pub const IO_REPARSE_TAG_JUNCTION_POINT_VALUE: u32 = 0xA0000003;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FollowPolicy {
    pub follow_mount_points: bool,
    pub follow_symbolic_links: bool,
    pub follow_junctions: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TraversalChildCandidate {
    pub full_path: Vec<u16>,
    pub reparse_tag: u32,
    pub is_protected_reparse_point: bool,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum TraversalSkipReason {
    ProtectedReparsePoint,
    MountPointNotFollowed,
    SymbolicLinkNotFollowed,
    JunctionNotFollowed,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TraversalSkip {
    pub full_path: Vec<u16>,
    pub reason: TraversalSkipReason,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TraversalPlan {
    pub enqueue_paths: Vec<Vec<u16>>,
    pub skipped_paths: Vec<TraversalSkip>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveryReportOutcome {
    pub scheduled_paths: Vec<Vec<u16>>,
}

pub struct ScanCoordinator {
    follow_policy: FollowPolicy,
    pending_paths: VecDeque<Vec<u16>>,
    tracked_paths: HashSet<Vec<u16>>,
}

impl ScanCoordinator {
    pub fn new() -> Self {
        Self {
            follow_policy: FollowPolicy {
                follow_mount_points: false,
                follow_symbolic_links: false,
                follow_junctions: false,
            },
            pending_paths: VecDeque::new(),
            tracked_paths: HashSet::new(),
        }
    }

    pub fn start(&mut self, root_path: Vec<u16>, follow_policy: FollowPolicy) {
        self.follow_policy = follow_policy;
        self.pending_paths.clear();
        self.tracked_paths.clear();
        self.tracked_paths.insert(root_path.clone());
        self.pending_paths.push_back(root_path);
    }

    pub fn clear(&mut self) {
        self.pending_paths.clear();
        self.tracked_paths.clear();
    }

    pub fn add_path(&mut self, path: Vec<u16>) -> bool {
        if !self.tracked_paths.insert(path.clone()) {
            return false;
        }

        self.pending_paths.push_back(path);
        true
    }

    pub fn take_next(&mut self) -> Option<Vec<u16>> {
        self.pending_paths.pop_front()
    }

    pub fn pending_count(&self) -> usize {
        self.pending_paths.len()
    }

    pub fn is_done(&self) -> bool {
        self.pending_paths.is_empty()
    }

    pub fn report_discovery_result(
        &mut self,
        child_candidates: &[TraversalChildCandidate],
    ) -> DiscoveryReportOutcome {
        let plan = plan_traversal(self.follow_policy, child_candidates);
        let mut scheduled_paths = Vec::new();
        for path in &plan.enqueue_paths {
            if self.add_path(path.clone()) {
                scheduled_paths.push(path.clone());
            }
        }

        DiscoveryReportOutcome { scheduled_paths }
    }
}

pub fn plan_traversal(
    follow_policy: FollowPolicy,
    child_candidates: &[TraversalChildCandidate],
) -> TraversalPlan {
    let mut enqueue_paths = Vec::new();
    let mut skipped_paths = Vec::new();

    for candidate in child_candidates {
        if let Some(reason) = traversal_skip_reason(follow_policy, candidate) {
            skipped_paths.push(TraversalSkip {
                full_path: candidate.full_path.clone(),
                reason,
            });
            continue;
        }

        enqueue_paths.push(candidate.full_path.clone());
    }

    TraversalPlan {
        enqueue_paths,
        skipped_paths,
    }
}

pub fn traversal_skip_reason(
    follow_policy: FollowPolicy,
    candidate: &TraversalChildCandidate,
) -> Option<TraversalSkipReason> {
    if candidate.is_protected_reparse_point {
        return Some(TraversalSkipReason::ProtectedReparsePoint);
    }

    if candidate.reparse_tag == IO_REPARSE_TAG_MOUNT_POINT_VALUE {
        return (!follow_policy.follow_mount_points)
            .then_some(TraversalSkipReason::MountPointNotFollowed);
    } else if candidate.reparse_tag == IO_REPARSE_TAG_SYMLINK_VALUE {
        return (!follow_policy.follow_symbolic_links)
            .then_some(TraversalSkipReason::SymbolicLinkNotFollowed);
    } else if candidate.reparse_tag == IO_REPARSE_TAG_JUNCTION_POINT_VALUE {
        return (!follow_policy.follow_junctions)
            .then_some(TraversalSkipReason::JunctionNotFollowed);
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wide(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    fn policy(
        follow_mount_points: bool,
        follow_symbolic_links: bool,
        follow_junctions: bool,
    ) -> FollowPolicy {
        FollowPolicy {
            follow_mount_points,
            follow_symbolic_links,
            follow_junctions,
        }
    }

    fn candidate(reparse_tag: u32, is_protected_reparse_point: bool) -> TraversalChildCandidate {
        TraversalChildCandidate {
            full_path: Vec::new(),
            reparse_tag,
            is_protected_reparse_point,
        }
    }

    fn path_candidate(
        full_path: &[u16],
        reparse_tag: u32,
        is_protected_reparse_point: bool,
    ) -> TraversalChildCandidate {
        TraversalChildCandidate {
            full_path: full_path.to_vec(),
            reparse_tag,
            is_protected_reparse_point,
        }
    }

    #[test]
    fn traversal_allows_non_reparse_and_unknown_reparse_tags() {
        assert!(traversal_skip_reason(policy(false, false, false), &candidate(0, false)).is_none());
        assert!(
            traversal_skip_reason(policy(false, false, false), &candidate(0x8000_1234, false))
                .is_none()
        );
    }

    #[test]
    fn traversal_skips_protected_reparse_points_before_follow_policy() {
        assert!(matches!(
            traversal_skip_reason(
                policy(true, true, true),
                &candidate(IO_REPARSE_TAG_SYMLINK_VALUE, true)
            ),
            Some(TraversalSkipReason::ProtectedReparsePoint)
        ));
    }

    #[test]
    fn traversal_respects_mount_point_follow_policy() {
        assert!(matches!(
            traversal_skip_reason(
                policy(false, true, true),
                &candidate(IO_REPARSE_TAG_MOUNT_POINT_VALUE, false)
            ),
            Some(TraversalSkipReason::MountPointNotFollowed)
        ));
        assert!(traversal_skip_reason(
            policy(true, false, false),
            &candidate(IO_REPARSE_TAG_MOUNT_POINT_VALUE, false)
        )
        .is_none());
    }

    #[test]
    fn traversal_planner_returns_stable_plain_output_for_mixed_inputs() {
        let normal = wide("C:\\root\\normal");
        let protected = wide("C:\\root\\protected");
        let symlink = wide("C:\\root\\symlink");
        let mount = wide("C:\\root\\mount");
        let unknown = wide("C:\\root\\unknown");

        let candidates = [
            path_candidate(&normal, 0, false),
            path_candidate(&protected, 0, true),
            path_candidate(&symlink, IO_REPARSE_TAG_SYMLINK_VALUE, false),
            path_candidate(&mount, IO_REPARSE_TAG_MOUNT_POINT_VALUE, false),
            path_candidate(&unknown, 0x8000_1234, false),
        ];

        let plan = plan_traversal(policy(false, false, false), &candidates);

        assert_eq!(plan.enqueue_paths, vec![normal, unknown]);
        assert_eq!(
            plan.skipped_paths,
            vec![
                TraversalSkip {
                    full_path: protected,
                    reason: TraversalSkipReason::ProtectedReparsePoint,
                },
                TraversalSkip {
                    full_path: symlink,
                    reason: TraversalSkipReason::SymbolicLinkNotFollowed,
                },
                TraversalSkip {
                    full_path: mount,
                    reason: TraversalSkipReason::MountPointNotFollowed,
                },
            ]
        );
    }

    #[test]
    fn traversal_planner_enqueues_symlink_when_follow_flag_is_set() {
        let symlink = wide("C:\\root\\symlink");
        let candidates = [path_candidate(
            &symlink,
            IO_REPARSE_TAG_SYMLINK_VALUE,
            false,
        )];

        let plan = plan_traversal(policy(false, true, false), &candidates);

        assert_eq!(plan.enqueue_paths, vec![symlink]);
        assert!(plan.skipped_paths.is_empty());
    }

    #[test]
    fn traversal_planner_preserves_mount_point_precedence_for_junction_tag_alias() {
        let mount = wide("C:\\root\\mount");
        let candidates = [path_candidate(
            &mount,
            IO_REPARSE_TAG_MOUNT_POINT_VALUE,
            false,
        )];

        let skipped = plan_traversal(policy(false, false, true), &candidates);
        assert_eq!(
            skipped.skipped_paths,
            vec![TraversalSkip {
                full_path: mount.clone(),
                reason: TraversalSkipReason::MountPointNotFollowed,
            }]
        );

        let enqueued = plan_traversal(policy(true, false, false), &candidates);
        assert_eq!(enqueued.enqueue_paths, vec![mount]);
        assert!(enqueued.skipped_paths.is_empty());
    }

    #[test]
    fn traversal_respects_symbolic_link_follow_policy() {
        assert!(matches!(
            traversal_skip_reason(
                policy(true, false, true),
                &candidate(IO_REPARSE_TAG_SYMLINK_VALUE, false)
            ),
            Some(TraversalSkipReason::SymbolicLinkNotFollowed)
        ));
        assert!(traversal_skip_reason(
            policy(false, true, false),
            &candidate(IO_REPARSE_TAG_SYMLINK_VALUE, false)
        )
        .is_none());
    }

    #[test]
    fn scan_coordinator_starts_with_root_as_next_work() {
        let root = wide("C:\\root");
        let mut coordinator = ScanCoordinator::new();

        coordinator.start(root.clone(), policy(false, false, false));

        assert_eq!(coordinator.pending_count(), 1);
        assert!(!coordinator.is_done());
        assert_eq!(coordinator.take_next(), Some(root));
        assert_eq!(coordinator.pending_count(), 0);
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_progresses_from_next_work_through_reported_children() {
        let root = wide("C:\\root");
        let normal_child = wide("C:\\root\\normal");
        let protected_child = wide("C:\\root\\protected");
        let symlink_child = wide("C:\\root\\link");

        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));
        assert_eq!(coordinator.take_next(), Some(root));

        let candidates = [
            path_candidate(&normal_child, 0, false),
            path_candidate(&protected_child, 0, true),
            path_candidate(&symlink_child, IO_REPARSE_TAG_SYMLINK_VALUE, false),
        ];

        let outcome = coordinator.report_discovery_result(&candidates);

        assert_eq!(outcome.scheduled_paths, vec![normal_child.clone()]);
        assert!(!coordinator.is_done());
        assert_eq!(coordinator.pending_count(), 1);
        assert_eq!(coordinator.take_next(), Some(normal_child));
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_restart_replaces_pending_and_tracked_state() {
        let first_root = wide("C:\\first");
        let first_child = wide("C:\\first\\child");
        let second_root = wide("C:\\second");

        let mut coordinator = ScanCoordinator::new();
        coordinator.start(first_root.clone(), policy(false, false, false));
        assert_eq!(coordinator.take_next(), Some(first_root.clone()));
        assert_eq!(
            coordinator
                .report_discovery_result(&[path_candidate(&first_child, 0, false)])
                .scheduled_paths,
            vec![first_child.clone()]
        );
        assert_eq!(coordinator.pending_count(), 1);

        coordinator.start(second_root.clone(), policy(false, false, false));

        assert_eq!(coordinator.pending_count(), 1);
        assert_eq!(coordinator.take_next(), Some(second_root));
        assert!(coordinator.add_path(first_root));
        assert!(coordinator.add_path(first_child));
        assert_eq!(coordinator.pending_count(), 2);
    }

    #[test]
    fn scan_coordinator_does_not_reenqueue_already_tracked_paths() {
        let root = wide("C:\\root");
        let child = wide("C:\\root\\child");
        let duplicate_child = child.clone();
        let candidates = [
            path_candidate(&child, 0, false),
            path_candidate(&duplicate_child, 0, false),
            path_candidate(&root, 0, false),
        ];

        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));
        assert_eq!(coordinator.take_next(), Some(root));

        assert_eq!(
            coordinator
                .report_discovery_result(&candidates)
                .scheduled_paths,
            vec![child.clone()]
        );
        assert_eq!(coordinator.pending_count(), 1);
        assert_eq!(coordinator.take_next(), Some(child));
        assert_eq!(coordinator.take_next(), None);
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_suppresses_duplicate_children_across_reports() {
        let root = wide("C:\\root");
        let child = wide("C:\\root\\child");
        let sibling = wide("C:\\root\\sibling");

        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));
        assert_eq!(coordinator.take_next(), Some(root));

        let first_report = [path_candidate(&child, 0, false)];
        assert_eq!(
            coordinator
                .report_discovery_result(&first_report)
                .scheduled_paths,
            vec![child.clone()]
        );

        let second_report = [
            path_candidate(&child, 0, false),
            path_candidate(&sibling, 0, false),
        ];
        assert_eq!(
            coordinator
                .report_discovery_result(&second_report)
                .scheduled_paths,
            vec![sibling.clone()]
        );

        assert_eq!(coordinator.pending_count(), 2);
        assert_eq!(coordinator.take_next(), Some(child));
        assert_eq!(coordinator.take_next(), Some(sibling));
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_report_preserves_new_scheduling_order_after_skips_and_duplicates() {
        let root = wide("C:\\root");
        let first = wide("C:\\root\\first");
        let duplicate = wide("C:\\root\\first");
        let protected = wide("C:\\root\\protected");
        let second = wide("C:\\root\\second");
        let unknown = wide("C:\\root\\unknown");

        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));
        assert_eq!(coordinator.take_next(), Some(root));

        let candidates = [
            path_candidate(&first, 0, false),
            path_candidate(&duplicate, 0, false),
            path_candidate(&protected, 0, true),
            path_candidate(&second, 0, false),
            path_candidate(&unknown, 0x8000_1234, false),
        ];

        assert_eq!(
            coordinator
                .report_discovery_result(&candidates)
                .scheduled_paths,
            vec![first.clone(), second.clone(), unknown.clone()]
        );
        assert_eq!(coordinator.take_next(), Some(first));
        assert_eq!(coordinator.take_next(), Some(second));
        assert_eq!(coordinator.take_next(), Some(unknown));
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_external_add_reports_duplicate_as_ignored() {
        let root = wide("C:\\root");
        let child = wide("C:\\root\\child");

        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));

        assert!(!coordinator.add_path(root));
        assert!(coordinator.add_path(child.clone()));
        assert!(!coordinator.add_path(child.clone()));
        assert_eq!(coordinator.pending_count(), 2);
        assert_eq!(coordinator.take_next(), Some(wide("C:\\root")));
        assert_eq!(coordinator.take_next(), Some(child));
        assert_eq!(coordinator.take_next(), None);
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_reports_no_next_work_when_pending_queue_is_empty() {
        let root = wide("C:\\root");
        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));

        assert_eq!(coordinator.take_next(), Some(root));
        assert_eq!(coordinator.take_next(), None);
        assert_eq!(coordinator.pending_count(), 0);
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_empty_directory_completion_does_not_schedule_work() {
        let root = wide("C:\\root");
        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));

        assert_eq!(coordinator.take_next(), Some(root));

        let scheduled = coordinator.report_discovery_result(&[]).scheduled_paths;

        assert!(scheduled.is_empty());
        assert_eq!(coordinator.pending_count(), 0);
        assert_eq!(coordinator.take_next(), None);
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_respects_symlink_follow_policy() {
        let root = wide("C:\\root");
        let symlink_child = wide("C:\\root\\link");
        let candidates = [path_candidate(
            &symlink_child,
            IO_REPARSE_TAG_SYMLINK_VALUE,
            false,
        )];

        let mut blocked = ScanCoordinator::new();
        blocked.start(root.clone(), policy(false, false, false));
        assert_eq!(blocked.take_next(), Some(root.clone()));
        assert!(blocked
            .report_discovery_result(&candidates)
            .scheduled_paths
            .is_empty());
        assert_eq!(blocked.pending_count(), 0);
        assert!(blocked.is_done());

        let mut followed = ScanCoordinator::new();
        followed.start(root.clone(), policy(false, true, false));
        assert_eq!(followed.take_next(), Some(root));
        assert_eq!(
            followed
                .report_discovery_result(&candidates)
                .scheduled_paths,
            vec![symlink_child.clone()]
        );
        assert!(!followed.is_done());
        assert_eq!(followed.take_next(), Some(symlink_child));
        assert!(followed.is_done());
    }

    #[test]
    fn scan_coordinator_preserves_mount_point_precedence_for_junction_tag_alias() {
        let root = wide("C:\\root");
        let mount_child = wide("C:\\root\\mount");
        let candidates = [path_candidate(
            &mount_child,
            IO_REPARSE_TAG_MOUNT_POINT_VALUE,
            false,
        )];

        let mut blocked_by_mount_policy = ScanCoordinator::new();
        blocked_by_mount_policy.start(root.clone(), policy(false, true, true));
        assert_eq!(blocked_by_mount_policy.take_next(), Some(root.clone()));
        assert!(blocked_by_mount_policy
            .report_discovery_result(&candidates)
            .scheduled_paths
            .is_empty());
        assert_eq!(blocked_by_mount_policy.pending_count(), 0);

        let mut allowed_by_mount_policy = ScanCoordinator::new();
        allowed_by_mount_policy.start(root.clone(), policy(true, false, false));
        assert_eq!(allowed_by_mount_policy.take_next(), Some(root));
        assert_eq!(
            allowed_by_mount_policy
                .report_discovery_result(&candidates)
                .scheduled_paths,
            vec![mount_child.clone()]
        );
        assert_eq!(allowed_by_mount_policy.take_next(), Some(mount_child));
        assert!(allowed_by_mount_policy.is_done());
    }

    #[test]
    fn scan_coordinator_keeps_unknown_reparse_and_non_reparse_directories_eligible() {
        let root = wide("C:\\root");
        let normal_child = wide("C:\\root\\normal");
        let unknown_child = wide("C:\\root\\unknown");
        let candidates = [
            path_candidate(&normal_child, 0, false),
            path_candidate(&unknown_child, 0x8000_1234, false),
        ];

        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));
        assert_eq!(coordinator.take_next(), Some(root));

        assert_eq!(
            coordinator
                .report_discovery_result(&candidates)
                .scheduled_paths,
            vec![normal_child.clone(), unknown_child.clone()]
        );
        assert_eq!(coordinator.pending_count(), 2);
        assert_eq!(coordinator.take_next(), Some(normal_child));
        assert_eq!(coordinator.take_next(), Some(unknown_child));
        assert_eq!(coordinator.take_next(), None);
        assert!(coordinator.is_done());
    }

    #[test]
    fn scan_coordinator_done_state_requires_pending_queue_to_drain() {
        let root = wide("C:\\root");
        let child = wide("C:\\root\\child");
        let candidates = [path_candidate(&child, 0, false)];

        let mut coordinator = ScanCoordinator::new();
        coordinator.start(root.clone(), policy(false, false, false));
        assert_eq!(coordinator.pending_count(), 1);
        assert!(!coordinator.is_done());

        assert_eq!(coordinator.take_next(), Some(root));
        assert_eq!(coordinator.pending_count(), 0);
        assert!(coordinator.is_done());

        assert_eq!(
            coordinator
                .report_discovery_result(&candidates)
                .scheduled_paths,
            vec![child.clone()]
        );
        assert_eq!(coordinator.pending_count(), 1);
        assert!(!coordinator.is_done());

        assert_eq!(coordinator.take_next(), Some(child));
        assert_eq!(coordinator.pending_count(), 0);
        assert_eq!(coordinator.take_next(), None);
        assert!(coordinator.is_done());
    }
}
