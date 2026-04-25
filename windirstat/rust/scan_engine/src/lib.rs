mod discovery;
mod lifecycle;
mod scan;
mod session;

use std::cell::RefCell;
use std::ffi::{c_char, c_void, CString};
use std::panic::AssertUnwindSafe;
use std::ptr;

pub use discovery::{discover_directory, DiscoveryEntry, DiscoveryEntryType};

#[repr(C)]
#[derive(Copy, Clone)]
pub enum RustDiscoveryEntryKind {
    File = 0,
    Directory = 1,
}

#[repr(C)]
pub struct RustDiscoveryEntryView {
    kind: RustDiscoveryEntryKind,
    name: *const u16,
    name_len: usize,
    full_path: *const u16,
    full_path_len: usize,
    size_physical: u64,
    size_logical: u64,
    index: u64,
    last_write_time: u64,
    attributes: u32,
    reparse_tag: u32,
    is_reserved: u8,
    is_off_volume: u8,
    is_protected_reparse_point: u8,
}

#[repr(C)]
pub struct RustDiscoveryErrorView {
    message: *const c_char,
    message_len: usize,
}

type RustDiscoveryEntryCallback =
    Option<unsafe extern "C" fn(entry: *const RustDiscoveryEntryView, user_data: *mut c_void)>;
type RustDiscoveryLogCallback =
    Option<unsafe extern "C" fn(message: *const u16, message_len: usize, user_data: *mut c_void)>;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RustScanSessionOptionsView {
    follow_mount_points: u8,
    follow_symbolic_links: u8,
    follow_junctions: u8,
}

#[repr(C)]
pub struct RustScanSessionChildCandidateView {
    full_path: *const u16,
    full_path_len: usize,
    reparse_tag: u32,
    is_protected_reparse_point: u8,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RustScanSessionPathView {
    path: *const u16,
    path_len: usize,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub enum RustScanSessionActionKind {
    RequestDirectory = 0,
    EmitDirectoryResult = 1,
    WaitingForInput = 2,
    Complete = 3,
    Cancelled = 4,
    Failed = 5,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RustScanSessionActionView {
    kind: RustScanSessionActionKind,
    path: RustScanSessionPathView,
    // Contract: for EmitDirectoryResult, Rust has already discovered the
    // directory and emitted the full entry batch through the callback; C++
    // only needs this count for logging/audit and RPC forwarding.
    scheduled_child_count: usize,
    cancel_reason: RustLifecycleTerminalReason,
    failure: RustLifecycleFailureView,
}

type RustScanSessionPathCallback =
    Option<unsafe extern "C" fn(path: *const RustScanSessionPathView, user_data: *mut c_void)>;

pub struct RustScanSession {
    inner: session::ScanSession,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RustScanSessionTimingStatsView {
    discovery_worker_elapsed_ns: u64,
    scheduling_elapsed_ns: u64,
    no_event_poll_count: u64,
    worker_count: usize,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum RustLifecycleTerminalReason {
    UserCancel = 1,
    Restarted = 2,
    EngineInterrupted = 3,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum RustLifecycleTerminalKind {
    Completed = 0,
    Canceled = 1,
    Failed = 2,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RustLifecycleFailureView {
    path: *const u16,
    path_len: usize,
    message: *const u16,
    message_len: usize,
    error_code: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RustLifecycleTerminalDecisionView {
    kind: RustLifecycleTerminalKind,
    cancel_reason: RustLifecycleTerminalReason,
    failure: RustLifecycleFailureView,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RustLifecycleSnapshotView {
    outstanding_work: u64,
    input_closed: u8,
    cancel_requested: u8,
    terminal_reached: u8,
    terminal_decision_taken: u8,
    cancel_reason: RustLifecycleTerminalReason,
    has_terminal_decision: u8,
    terminal_decision: RustLifecycleTerminalDecisionView,
}

thread_local! {
    static LAST_ERROR_MESSAGE: RefCell<Option<CString>> = const { RefCell::new(None) };
    static LIFECYCLE_FFI_STRINGS: RefCell<Vec<Box<[u16]>>> = const { RefCell::new(Vec::new()) };
}

fn utf16_slice_to_string(value: *const u16, len: usize) -> Result<String, &'static str> {
    if value.is_null() {
        return Err("Path pointer was null.");
    }

    let slice = unsafe { std::slice::from_raw_parts(value, len) };
    Ok(String::from_utf16_lossy(slice))
}

fn copy_utf16_slice(value: *const u16, len: usize) -> Result<Vec<u16>, &'static str> {
    if value.is_null() {
        return Err("Path pointer was null.");
    }

    let slice = unsafe { std::slice::from_raw_parts(value, len) };
    Ok(slice.to_vec())
}

fn follow_policy_from_ffi(value: RustScanSessionOptionsView) -> scan::FollowPolicy {
    scan::FollowPolicy {
        follow_mount_points: value.follow_mount_points != 0,
        follow_symbolic_links: value.follow_symbolic_links != 0,
        follow_junctions: value.follow_junctions != 0,
    }
}

fn session_candidates_from_ffi(
    child_candidates: *const RustScanSessionChildCandidateView,
    child_candidate_count: usize,
) -> Result<Vec<scan::TraversalChildCandidate>, &'static str> {
    if child_candidate_count != 0 && child_candidates.is_null() {
        return Err("Scan coordinator candidate pointer was null.");
    }

    let candidates = if child_candidate_count == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(child_candidates, child_candidate_count) }
    };

    candidates
        .iter()
        .map(|candidate| {
            Ok(scan::TraversalChildCandidate {
                full_path: copy_utf16_slice(candidate.full_path, candidate.full_path_len)?,
                reparse_tag: candidate.reparse_tag,
                is_protected_reparse_point: candidate.is_protected_reparse_point != 0,
            })
        })
        .collect()
}

fn lifecycle_reason_from_ffi(value: RustLifecycleTerminalReason) -> lifecycle::TerminalReason {
    match value {
        RustLifecycleTerminalReason::UserCancel => lifecycle::TerminalReason::UserCancel,
        RustLifecycleTerminalReason::Restarted => lifecycle::TerminalReason::Restarted,
        RustLifecycleTerminalReason::EngineInterrupted => {
            lifecycle::TerminalReason::EngineInterrupted
        }
    }
}

fn lifecycle_reason_to_ffi(value: lifecycle::TerminalReason) -> RustLifecycleTerminalReason {
    match value {
        lifecycle::TerminalReason::UserCancel => RustLifecycleTerminalReason::UserCancel,
        lifecycle::TerminalReason::Restarted => RustLifecycleTerminalReason::Restarted,
        lifecycle::TerminalReason::EngineInterrupted => {
            RustLifecycleTerminalReason::EngineInterrupted
        }
    }
}

fn lifecycle_terminal_kind_to_ffi(value: lifecycle::TerminalKind) -> RustLifecycleTerminalKind {
    match value {
        lifecycle::TerminalKind::Completed => RustLifecycleTerminalKind::Completed,
        lifecycle::TerminalKind::Canceled => RustLifecycleTerminalKind::Canceled,
        lifecycle::TerminalKind::Failed => RustLifecycleTerminalKind::Failed,
    }
}

fn lifecycle_failure_from_ffi(
    value: RustLifecycleFailureView,
) -> Result<lifecycle::LifecycleFailure, &'static str> {
    Ok(lifecycle::LifecycleFailure {
        path: if value.path_len == 0 {
            Vec::new()
        } else {
            copy_utf16_slice(value.path, value.path_len)?
        },
        message: if value.message_len == 0 {
            Vec::new()
        } else {
            copy_utf16_slice(value.message, value.message_len)?
        },
        error_code: value.error_code,
    })
}

fn lifecycle_failure_to_ffi(value: &lifecycle::LifecycleFailure) -> RustLifecycleFailureView {
    let (path, path_len) = store_lifecycle_ffi_string(&value.path);
    let (message, message_len) = store_lifecycle_ffi_string(&value.message);
    RustLifecycleFailureView {
        path,
        path_len,
        message,
        message_len,
        error_code: value.error_code,
    }
}

fn clear_lifecycle_ffi_strings() {
    LIFECYCLE_FFI_STRINGS.with(|storage| {
        storage.borrow_mut().clear();
    });
}

fn store_lifecycle_ffi_string(value: &[u16]) -> (*const u16, usize) {
    if value.is_empty() {
        return (ptr::null(), 0);
    }

    LIFECYCLE_FFI_STRINGS.with(|storage| {
        let mut storage = storage.borrow_mut();
        storage.push(value.to_vec().into_boxed_slice());
        let stored = storage.last().expect("just pushed lifecycle FFI string");
        (stored.as_ptr(), stored.len())
    })
}

fn empty_lifecycle_failure_view() -> RustLifecycleFailureView {
    RustLifecycleFailureView {
        path: ptr::null(),
        path_len: 0,
        message: ptr::null(),
        message_len: 0,
        error_code: 0,
    }
}

fn empty_scan_session_path_view() -> RustScanSessionPathView {
    RustScanSessionPathView {
        path: ptr::null(),
        path_len: 0,
    }
}

fn empty_lifecycle_terminal_decision_view() -> RustLifecycleTerminalDecisionView {
    RustLifecycleTerminalDecisionView {
        kind: RustLifecycleTerminalKind::Completed,
        cancel_reason: RustLifecycleTerminalReason::EngineInterrupted,
        failure: empty_lifecycle_failure_view(),
    }
}

fn lifecycle_terminal_decision_to_ffi(
    value: &lifecycle::TerminalDecision,
) -> RustLifecycleTerminalDecisionView {
    RustLifecycleTerminalDecisionView {
        kind: lifecycle_terminal_kind_to_ffi(value.kind),
        cancel_reason: lifecycle_reason_to_ffi(value.cancel_reason),
        failure: lifecycle_failure_to_ffi(&value.failure),
    }
}

fn session_action_to_ffi(value: session::ScanSessionAction) -> RustScanSessionActionView {
    match value {
        session::ScanSessionAction::RequestDirectory(path) => {
            let (path_ptr, path_len) = store_lifecycle_ffi_string(&path);
            RustScanSessionActionView {
                kind: RustScanSessionActionKind::RequestDirectory,
                path: RustScanSessionPathView {
                    path: path_ptr,
                    path_len,
                },
                scheduled_child_count: 0,
                cancel_reason: RustLifecycleTerminalReason::EngineInterrupted,
                failure: empty_lifecycle_failure_view(),
            }
        }
        session::ScanSessionAction::EmitDirectoryResult(result) => {
            let (path_ptr, path_len) = store_lifecycle_ffi_string(&result.directory_path);
            RustScanSessionActionView {
                kind: RustScanSessionActionKind::EmitDirectoryResult,
                path: RustScanSessionPathView {
                    path: path_ptr,
                    path_len,
                },
                scheduled_child_count: result.scheduled_paths.len(),
                cancel_reason: RustLifecycleTerminalReason::EngineInterrupted,
                failure: empty_lifecycle_failure_view(),
            }
        }
        session::ScanSessionAction::WaitingForInput => RustScanSessionActionView {
            kind: RustScanSessionActionKind::WaitingForInput,
            path: empty_scan_session_path_view(),
            scheduled_child_count: 0,
            cancel_reason: RustLifecycleTerminalReason::EngineInterrupted,
            failure: empty_lifecycle_failure_view(),
        },
        session::ScanSessionAction::Complete => RustScanSessionActionView {
            kind: RustScanSessionActionKind::Complete,
            path: empty_scan_session_path_view(),
            scheduled_child_count: 0,
            cancel_reason: RustLifecycleTerminalReason::EngineInterrupted,
            failure: empty_lifecycle_failure_view(),
        },
        session::ScanSessionAction::Cancelled(reason) => RustScanSessionActionView {
            kind: RustScanSessionActionKind::Cancelled,
            path: empty_scan_session_path_view(),
            scheduled_child_count: 0,
            cancel_reason: lifecycle_reason_to_ffi(reason),
            failure: empty_lifecycle_failure_view(),
        },
        session::ScanSessionAction::Failed(failure) => RustScanSessionActionView {
            kind: RustScanSessionActionKind::Failed,
            path: empty_scan_session_path_view(),
            scheduled_child_count: 0,
            cancel_reason: RustLifecycleTerminalReason::EngineInterrupted,
            failure: lifecycle_failure_to_ffi(&failure),
        },
    }
}

fn lifecycle_snapshot_to_ffi(value: &lifecycle::LifecycleSnapshot) -> RustLifecycleSnapshotView {
    let terminal_decision = value
        .terminal_decision
        .as_ref()
        .map(lifecycle_terminal_decision_to_ffi)
        .unwrap_or_else(empty_lifecycle_terminal_decision_view);

    RustLifecycleSnapshotView {
        outstanding_work: value.outstanding_work,
        input_closed: value.input_closed as u8,
        cancel_requested: value.cancel_requested as u8,
        terminal_reached: value.terminal_reached as u8,
        terminal_decision_taken: value.terminal_decision_taken as u8,
        cancel_reason: lifecycle_reason_to_ffi(value.cancel_reason),
        has_terminal_decision: value.terminal_decision.is_some() as u8,
        terminal_decision,
    }
}

fn set_error(error: *mut RustDiscoveryErrorView, message: &str) {
    if error.is_null() {
        return;
    }

    let sanitized = message.replace('\0', " ");
    LAST_ERROR_MESSAGE.with(|slot| {
        let fallback = CString::new("Rust discovery error").expect("static CString creation");
        let cstring = CString::new(sanitized).unwrap_or(fallback);
        let len = cstring.to_bytes().len();
        let ptr = cstring.as_ptr();
        *slot.borrow_mut() = Some(cstring);
        unsafe {
            (*error).message = ptr;
            (*error).message_len = len;
        }
    });
}

fn clear_error(error: *mut RustDiscoveryErrorView) {
    LAST_ERROR_MESSAGE.with(|slot| {
        *slot.borrow_mut() = None;
    });

    if error.is_null() {
        return;
    }

    unsafe {
        (*error).message = ptr::null();
        (*error).message_len = 0;
    }
}

fn emit_log(callback: RustDiscoveryLogCallback, user_data: *mut c_void, message: &str) {
    let Some(callback) = callback else {
        return;
    };

    let wide: Vec<u16> = message.replace('\0', " ").encode_utf16().collect();
    unsafe { callback(wide.as_ptr(), wide.len(), user_data) };
}

fn emit_discovery_entry(
    callback: RustDiscoveryEntryCallback,
    user_data: *mut c_void,
    entry: &discovery::DiscoveredEntry,
) {
    let Some(callback) = callback else {
        return;
    };

    let view = RustDiscoveryEntryView {
        kind: match entry.kind {
            discovery::DiscoveredEntryKind::File => RustDiscoveryEntryKind::File,
            discovery::DiscoveredEntryKind::Directory => RustDiscoveryEntryKind::Directory,
        },
        name: entry.name.as_ptr(),
        name_len: entry.name.len(),
        full_path: entry.full_path.as_ptr(),
        full_path_len: entry.full_path.len(),
        size_physical: entry.size_physical,
        size_logical: entry.size_logical,
        index: entry.index,
        last_write_time: entry.last_write_time,
        attributes: entry.attributes,
        reparse_tag: entry.reparse_tag,
        is_reserved: entry.is_reserved as u8,
        is_off_volume: entry.is_off_volume as u8,
        is_protected_reparse_point: entry.is_protected_reparse_point as u8,
    };
    unsafe { callback(&view, user_data) };
}

fn emit_path(callback: RustScanSessionPathCallback, user_data: *mut c_void, path: &[u16]) {
    let Some(callback) = callback else {
        return;
    };

    let view = RustScanSessionPathView {
        path: path.as_ptr(),
        path_len: path.len(),
    };
    unsafe { callback(&view, user_data) };
}

#[no_mangle]
pub extern "C" fn windirstat_rust_discover_directory(
    path: *const u16,
    path_len: usize,
    callback: RustDiscoveryEntryCallback,
    user_data: *mut c_void,
    log_callback: RustDiscoveryLogCallback,
    log_user_data: *mut c_void,
    error: *mut RustDiscoveryErrorView,
) -> bool {
    clear_error(error);

    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let path = utf16_slice_to_string(path, path_len).map_err(|message| message.to_string())?;
        discovery::enumerate_directory(
            &path,
            |entry| emit_discovery_entry(callback, user_data, entry),
            |message| emit_log(log_callback, log_user_data, message),
        )
    }));

    match result {
        Ok(Ok(())) => true,
        Ok(Err(message)) => {
            set_error(error, &message);
            false
        }
        Err(_) => {
            set_error(error, "Rust discovery panicked.");
            false
        }
    }
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_create() -> *mut RustScanSession {
    Box::into_raw(Box::new(RustScanSession {
        inner: session::ScanSession::new(),
    }))
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_destroy(session: *mut RustScanSession) {
    if session.is_null() {
        return;
    }

    unsafe {
        drop(Box::from_raw(session));
    }
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_start(
    session: *mut RustScanSession,
    root_path: *const u16,
    root_path_len: usize,
    options: RustScanSessionOptionsView,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() {
            return false;
        }

        let Ok(root_path) = copy_utf16_slice(root_path, root_path_len) else {
            return false;
        };
        let session = unsafe { &mut *session };
        session
            .inner
            .start(root_path, follow_policy_from_ffi(options));
        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_reset(session: *mut RustScanSession) {
    if session.is_null() {
        return;
    }

    let session = unsafe { &mut *session };
    session.inner.reset();
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_clear_pending_work(session: *mut RustScanSession) {
    if session.is_null() {
        return;
    }

    let session = unsafe { &mut *session };
    session.inner.clear_pending_work();
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_add_external_path(
    session: *mut RustScanSession,
    path: *const u16,
    path_len: usize,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() {
            return false;
        }

        let Ok(path) = copy_utf16_slice(path, path_len) else {
            return false;
        };
        let session = unsafe { &mut *session };
        session.inner.add_external_path(path)
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_close_input(session: *mut RustScanSession) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() {
            return false;
        }

        let session = unsafe { &mut *session };
        session.inner.close_input();
        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_request_cancel(
    session: *mut RustScanSession,
    reason: RustLifecycleTerminalReason,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() {
            return false;
        }

        let session = unsafe { &mut *session };
        session
            .inner
            .request_cancel(lifecycle_reason_from_ffi(reason));
        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_next_action(
    session: *mut RustScanSession,
    output: *mut RustScanSessionActionView,
    entry_callback: RustDiscoveryEntryCallback,
    user_data: *mut c_void,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() || output.is_null() {
            return false;
        }

        clear_lifecycle_ffi_strings();
        let session = unsafe { &mut *session };
        let action = session.inner.next_action();
        if let session::ScanSessionAction::EmitDirectoryResult(result) = &action {
            for entry in &result.discovered_entries {
                emit_discovery_entry(entry_callback, user_data, entry);
            }
        }
        unsafe {
            *output = session_action_to_ffi(action);
        }
        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_take_next_path(
    session: *mut RustScanSession,
    callback: RustScanSessionPathCallback,
    user_data: *mut c_void,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() {
            return false;
        }

        let session = unsafe { &mut *session };
        let Some(path) = session.inner.take_next_path() else {
            return false;
        };

        emit_path(callback, user_data, &path);
        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_report_directory_result(
    session: *mut RustScanSession,
    child_candidates: *const RustScanSessionChildCandidateView,
    child_candidate_count: usize,
    scheduled_callback: RustScanSessionPathCallback,
    user_data: *mut c_void,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() {
            return false;
        }

        let Ok(candidates) = session_candidates_from_ffi(child_candidates, child_candidate_count)
        else {
            return false;
        };

        let session = unsafe { &mut *session };
        let outcome = session.inner.report_directory_result(&candidates);
        for path in &outcome.scheduled_paths {
            emit_path(scheduled_callback, user_data, path);
        }

        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_report_directory_failure(
    session: *mut RustScanSession,
    failure: RustLifecycleFailureView,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() {
            return false;
        }

        let Ok(failure) = lifecycle_failure_from_ffi(failure) else {
            return false;
        };
        let session = unsafe { &mut *session };
        session.inner.report_directory_failure(failure);
        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_has_terminal_decision(
    session: *const RustScanSession,
) -> bool {
    if session.is_null() {
        return false;
    }

    let session = unsafe { &*session };
    session.inner.has_terminal_decision()
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_take_terminal_decision(
    session: *mut RustScanSession,
    output: *mut RustLifecycleTerminalDecisionView,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() || output.is_null() {
            return false;
        }

        clear_lifecycle_ffi_strings();
        let session = unsafe { &mut *session };
        let Some(decision) = session.inner.take_terminal_decision() else {
            return false;
        };
        unsafe {
            *output = lifecycle_terminal_decision_to_ffi(&decision);
        }
        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_snapshot(
    session: *const RustScanSession,
    output: *mut RustLifecycleSnapshotView,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() || output.is_null() {
            return false;
        }

        clear_lifecycle_ffi_strings();
        let session = unsafe { &*session };
        let snapshot = session.inner.lifecycle_snapshot();
        unsafe {
            *output = lifecycle_snapshot_to_ffi(&snapshot);
        }
        true
    }));

    result.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_pending_count(
    session: *const RustScanSession,
) -> usize {
    if session.is_null() {
        return 0;
    }

    let session = unsafe { &*session };
    session.inner.pending_work_count()
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_is_done(session: *const RustScanSession) -> bool {
    if session.is_null() {
        return true;
    }

    let session = unsafe { &*session };
    session.inner.is_done()
}

#[no_mangle]
pub extern "C" fn windirstat_rust_scan_session_timing_stats(
    session: *const RustScanSession,
    output: *mut RustScanSessionTimingStatsView,
) -> bool {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if session.is_null() || output.is_null() {
            return false;
        }

        let session = unsafe { &*session };
        let stats = session.inner.timing_stats();
        unsafe {
            *output = RustScanSessionTimingStatsView {
                discovery_worker_elapsed_ns: stats.discovery_worker_elapsed_ns,
                scheduling_elapsed_ns: stats.scheduling_elapsed_ns,
                no_event_poll_count: stats.no_event_poll_count,
                worker_count: stats.worker_count,
            };
        }
        true
    }));

    result.unwrap_or(false)
}
