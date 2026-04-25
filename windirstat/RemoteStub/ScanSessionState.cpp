#include "pch.h"
#include "RemoteStub/ScanSessionState.h"

#include "RemoteStub/RustScanSessionBridge.h"

#include <deque>
#include <unordered_set>

namespace
{
std::wstring CopyWideString(const std::uint16_t* value, const std::size_t length)
{
    if (value == nullptr || length == 0)
        return {};

    return std::wstring(reinterpret_cast<const wchar_t*>(value), length);
}

RustScanSessionTerminalReason ToRustReason(const ScanTerminalReason reason)
{
    switch (reason)
    {
    case ScanTerminalReason::UserCancel:
        return RustScanSessionTerminalReason::UserCancel;
    case ScanTerminalReason::Restarted:
        return RustScanSessionTerminalReason::Restarted;
    case ScanTerminalReason::EngineInterrupted:
    default:
        return RustScanSessionTerminalReason::EngineInterrupted;
    }
}

ScanTerminalReason FromRustReason(const RustScanSessionTerminalReason reason)
{
    switch (reason)
    {
    case RustScanSessionTerminalReason::UserCancel:
        return ScanTerminalReason::UserCancel;
    case RustScanSessionTerminalReason::Restarted:
        return ScanTerminalReason::Restarted;
    case RustScanSessionTerminalReason::EngineInterrupted:
    default:
        return ScanTerminalReason::EngineInterrupted;
    }
}

ScanSessionTerminalKind FromRustTerminalKind(const RustScanSessionTerminalKind kind)
{
    switch (kind)
    {
    case RustScanSessionTerminalKind::Canceled:
        return ScanSessionTerminalKind::Canceled;
    case RustScanSessionTerminalKind::Failed:
        return ScanSessionTerminalKind::Failed;
    case RustScanSessionTerminalKind::Completed:
    default:
        return ScanSessionTerminalKind::Completed;
    }
}

ScanSessionActionKind FromRustActionKind(const RustScanSessionActionKind kind)
{
    switch (kind)
    {
    case RustScanSessionActionKind::EmitDirectoryResult:
        return ScanSessionActionKind::EmitDirectoryResult;
    case RustScanSessionActionKind::RequestDirectory:
        return ScanSessionActionKind::RequestDirectory;
    case RustScanSessionActionKind::Complete:
        return ScanSessionActionKind::Complete;
    case RustScanSessionActionKind::Cancelled:
        return ScanSessionActionKind::Cancelled;
    case RustScanSessionActionKind::Failed:
        return ScanSessionActionKind::Failed;
    case RustScanSessionActionKind::WaitingForInput:
    default:
        return ScanSessionActionKind::WaitingForInput;
    }
}

RustScanSessionOptionsView ToRustOptions(const ScanSessionOptions& options)
{
    RustScanSessionOptionsView view{};
    view.followMountPoints = options.followMountPoints ? 1 : 0;
    view.followSymbolicLinks = options.followSymbolicLinks ? 1 : 0;
    view.followJunctions = options.followJunctions ? 1 : 0;
    return view;
}

RustScanSessionFailureView ToRustFailureView(const ScanSessionFailure& failure)
{
    RustScanSessionFailureView view{};
    view.path = reinterpret_cast<const std::uint16_t*>(failure.path.data());
    view.pathLen = failure.path.size();
    view.message = reinterpret_cast<const std::uint16_t*>(failure.message.data());
    view.messageLen = failure.message.size();
    view.errorCode = static_cast<std::uint32_t>(failure.errorCode);
    return view;
}

ScanSessionFailure FromRustFailureView(const RustScanSessionFailureView& view)
{
    ScanSessionFailure failure{};
    failure.path = CopyWideString(view.path, view.pathLen);
    failure.message = CopyWideString(view.message, view.messageLen);
    failure.errorCode = view.errorCode;
    return failure;
}

ScanSessionTerminalDecision FromRustDecisionView(const RustScanSessionTerminalDecisionView& view)
{
    ScanSessionTerminalDecision decision{};
    decision.kind = FromRustTerminalKind(view.kind);
    decision.cancelReason = FromRustReason(view.cancelReason);
    decision.failure = FromRustFailureView(view.failure);
    return decision;
}

FILETIME FileTimeFromU64(const std::uint64_t value)
{
    FILETIME fileTime{};
    fileTime.dwLowDateTime = static_cast<DWORD>(value & 0xFFFF'FFFFULL);
    fileTime.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return fileTime;
}

void AppendDiscoveredEntry(
    ScanSessionDirectoryResult& output,
    const RustDiscoveryEntryView& entry)
{
    const std::wstring name = CopyWideString(entry.name, entry.nameLen);
    const std::wstring fullPath = CopyWideString(entry.fullPath, entry.fullPathLen);
    const FILETIME lastChange = FileTimeFromU64(entry.lastWriteTime);
    if (entry.kind == RustDiscoveryEntryKind::File)
    {
        DiscoveredFile file{};
        file.name = name;
        file.fullPath = fullPath;
        file.sizePhysical = entry.sizePhysical;
        file.sizeLogical = entry.sizeLogical;
        file.index = entry.index;
        file.lastChange = lastChange;
        file.attributes = entry.attributes;
        file.reparseTag = entry.reparseTag;
        file.isReserved = entry.isReserved != 0;
        output.files.push_back(std::move(file));
        return;
    }

    DiscoveredDirectory directory{};
    directory.name = name;
    directory.fullPath = fullPath;
    directory.index = entry.index;
    directory.lastChange = lastChange;
    directory.attributes = entry.attributes;
    directory.reparseTag = entry.reparseTag;
    directory.isReserved = entry.isReserved != 0;
    directory.isOffVolume = entry.isOffVolume != 0;
    directory.isProtectedReparsePoint = entry.isProtectedReparsePoint != 0;
    output.directories.push_back(std::move(directory));
}

ScanSessionAction FromRustActionView(const RustScanSessionActionView& view)
{
    ScanSessionAction action{};
    action.kind = FromRustActionKind(view.kind);
    action.path = CopyWideString(view.path.path, view.path.pathLen);
    action.directoryResult.directoryPath = action.path;
    action.directoryResult.scheduledChildCount = view.scheduledChildCount;
    action.cancelReason = FromRustReason(view.cancelReason);
    action.failure = FromRustFailureView(view.failure);
    return action;
}

ScanLifecycleSnapshot FromRustSnapshotView(const RustScanSessionSnapshotView& view)
{
    ScanLifecycleSnapshot snapshot{};
    snapshot.outstandingWork = view.outstandingWork;
    snapshot.inputClosed = view.inputClosed != 0;
    snapshot.cancelRequested = view.cancelRequested != 0;
    snapshot.terminalReached = view.terminalReached != 0;
    snapshot.terminalDecisionTaken = view.terminalDecisionTaken != 0;
    snapshot.cancelReason = FromRustReason(view.cancelReason);
    if (view.hasTerminalDecision != 0)
        snapshot.terminalDecision = FromRustDecisionView(view.terminalDecision);
    return snapshot;
}

ScanSessionTimingStats FromRustTimingStatsView(const RustScanSessionTimingStatsView& view)
{
    ScanSessionTimingStats stats{};
    stats.discoveryWorkerElapsedNs = view.discoveryWorkerElapsedNs;
    stats.schedulingElapsedNs = view.schedulingElapsedNs;
    stats.noEventPollCount = view.noEventPollCount;
    stats.workerCount = view.workerCount;
    return stats;
}

std::optional<TraversalSkipReason> GetNativeTemporarySkipReason(
    const ScanSessionOptions& options,
    const TraversalChildCandidate& candidate)
{
    if (candidate.isProtectedReparsePoint)
        return TraversalSkipReason::ProtectedReparsePoint;

    if (candidate.reparseTag == IO_REPARSE_TAG_MOUNT_POINT)
        return options.followMountPoints
            ? std::optional<TraversalSkipReason>{}
            : std::optional<TraversalSkipReason>{ TraversalSkipReason::MountPointNotFollowed };
    if (candidate.reparseTag == IO_REPARSE_TAG_SYMLINK)
        return options.followSymbolicLinks
            ? std::optional<TraversalSkipReason>{}
            : std::optional<TraversalSkipReason>{ TraversalSkipReason::SymbolicLinkNotFollowed };
    if (candidate.reparseTag == IO_REPARSE_TAG_JUNCTION_POINT)
        return options.followJunctions
            ? std::optional<TraversalSkipReason>{}
            : std::optional<TraversalSkipReason>{ TraversalSkipReason::JunctionNotFollowed };

    return std::nullopt;
}

void CollectRustSessionPath(const RustScanSessionPathView* const path, void* const userData)
{
    ASSERT(path != nullptr);
    ASSERT(userData != nullptr);

    auto& output = *static_cast<std::vector<std::wstring>*>(userData);
    output.push_back(CopyWideString(path->path, path->pathLen));
}

void CollectRustDiscoveryEntry(const RustDiscoveryEntryView* const entry, void* const userData)
{
    ASSERT(entry != nullptr);
    ASSERT(userData != nullptr);

    auto& output = *static_cast<ScanSessionDirectoryResult*>(userData);
    AppendDiscoveredEntry(output, *entry);
}
}

// Temporary: this fallback mirrors the Rust session contract only for builds
// that do not link the Rust static library and for the host seam test binary.
// Removal condition: remove it when every supported scan-host test/build target
// links the Rust scan session directly.
class ScanSessionState::NativeSessionState
{
public:
    void Start(const std::wstring& rootPath, const ScanSessionOptions& options)
    {
        Reset();
        m_options = options;
        m_pendingPaths.push_back(rootPath);
        m_trackedPaths.insert(rootPath);
        m_outstandingWork = 1;
    }

    void Reset()
    {
        m_options = {};
        m_pendingPaths.clear();
        m_trackedPaths.clear();
        m_outstandingWork = 0;
        m_inputClosed = false;
        m_cancelRequested = false;
        m_terminalReached = false;
        m_terminalDecisionTaken = false;
        m_cancelReason = ScanTerminalReason::EngineInterrupted;
        m_terminalDecision.reset();
    }

    void ClearPendingWork()
    {
        m_pendingPaths.clear();
        m_trackedPaths.clear();
    }

    bool AddExternalPath(const std::wstring& path)
    {
        if (m_inputClosed || m_cancelRequested || m_terminalReached)
            return false;
        if (!m_trackedPaths.insert(path).second)
            return false;

        m_pendingPaths.push_back(path);
        ++m_outstandingWork;
        return true;
    }

    ScanSessionAction NextAction()
    {
        if (m_terminalDecision.has_value() && !m_terminalDecisionTaken)
        {
            ScanSessionAction action{};
            switch (m_terminalDecision->kind)
            {
            case ScanSessionTerminalKind::Completed:
                action.kind = ScanSessionActionKind::Complete;
                break;
            case ScanSessionTerminalKind::Canceled:
                action.kind = ScanSessionActionKind::Cancelled;
                action.cancelReason = m_terminalDecision->cancelReason;
                break;
            case ScanSessionTerminalKind::Failed:
                action.kind = ScanSessionActionKind::Failed;
                action.failure = m_terminalDecision->failure;
                break;
            default:
                ASSERT(false);
                action.kind = ScanSessionActionKind::WaitingForInput;
                break;
            }

            return action;
        }

        if (m_pendingPaths.empty())
            return ScanSessionAction{};

        ScanSessionAction action{};
        action.kind = ScanSessionActionKind::RequestDirectory;
        action.path = std::move(m_pendingPaths.front());
        m_pendingPaths.pop_front();
        return action;
    }

    std::optional<std::wstring> TryTakeNextPath()
    {
        if (m_terminalDecision.has_value() || m_pendingPaths.empty())
            return std::nullopt;

        std::wstring path = std::move(m_pendingPaths.front());
        m_pendingPaths.pop_front();
        return path;
    }

    ScanSessionDirectoryReport ReportDirectoryResult(const std::vector<TraversalChildCandidate>& childCandidates)
    {
        ScanSessionDirectoryReport report{};
        if (m_terminalReached)
            return report;

        for (const auto& candidate : childCandidates)
        {
            if (GetNativeTemporarySkipReason(m_options, candidate).has_value())
                continue;
            if (!m_trackedPaths.insert(candidate.fullPath).second)
                continue;

            m_pendingPaths.push_back(candidate.fullPath);
            report.scheduledChildPaths.push_back(candidate.fullPath);
        }

        m_outstandingWork += report.scheduledChildPaths.size();
        FinishOneOutstandingWork();
        return report;
    }

    void ReportDirectoryFailure(ScanSessionFailure failure)
    {
        if (m_terminalReached)
            return;

        ClearPendingWork();
        m_outstandingWork = 0;
        ScanSessionTerminalDecision decision{};
        decision.kind = ScanSessionTerminalKind::Failed;
        decision.failure = std::move(failure);
        ReachTerminal(std::move(decision));
    }

    void CloseInput()
    {
        if (m_terminalReached)
            return;

        m_inputClosed = true;
        TryReachCompleted();
    }

    void RequestCancel(const ScanTerminalReason reason)
    {
        if (m_terminalReached)
            return;

        ClearPendingWork();
        m_cancelRequested = true;
        m_inputClosed = true;
        m_cancelReason = reason;
        m_outstandingWork = 0;
        ScanSessionTerminalDecision decision{};
        decision.kind = ScanSessionTerminalKind::Canceled;
        decision.cancelReason = reason;
        ReachTerminal(std::move(decision));
    }

    std::optional<ScanSessionTerminalDecision> TakeTerminalDecision()
    {
        if (!m_terminalDecision.has_value() || m_terminalDecisionTaken)
            return std::nullopt;

        m_terminalDecisionTaken = true;
        return m_terminalDecision;
    }

    ScanLifecycleSnapshot Snapshot() const
    {
        ScanLifecycleSnapshot snapshot{};
        snapshot.outstandingWork = m_outstandingWork;
        snapshot.inputClosed = m_inputClosed;
        snapshot.cancelRequested = m_cancelRequested;
        snapshot.terminalReached = m_terminalReached;
        snapshot.terminalDecisionTaken = m_terminalDecisionTaken;
        snapshot.cancelReason = m_cancelReason;
        snapshot.terminalDecision = m_terminalDecision;
        return snapshot;
    }

    bool IsDone() const
    {
        return m_pendingPaths.empty();
    }

    std::size_t PendingWorkCount() const
    {
        return m_pendingPaths.size();
    }

private:
    void FinishOneOutstandingWork()
    {
        if (m_outstandingWork != 0)
            --m_outstandingWork;
        TryReachCompleted();
    }

    void TryReachCompleted()
    {
        if (m_terminalReached || !m_inputClosed || m_outstandingWork != 0)
            return;

        ScanSessionTerminalDecision decision{};
        decision.kind = ScanSessionTerminalKind::Completed;
        ReachTerminal(std::move(decision));
    }

    void ReachTerminal(ScanSessionTerminalDecision decision)
    {
        if (m_terminalReached)
            return;

        m_terminalReached = true;
        m_terminalDecision = std::move(decision);
    }

    ScanSessionOptions m_options{};
    std::deque<std::wstring> m_pendingPaths;
    std::unordered_set<std::wstring> m_trackedPaths;
    std::uint64_t m_outstandingWork = 0;
    bool m_inputClosed = false;
    bool m_cancelRequested = false;
    bool m_terminalReached = false;
    bool m_terminalDecisionTaken = false;
    ScanTerminalReason m_cancelReason = ScanTerminalReason::EngineInterrupted;
    std::optional<ScanSessionTerminalDecision> m_terminalDecision;
};

ScanSessionState::ScanSessionState(const bool enableRustSession)
    : m_nativeSession(std::make_unique<NativeSessionState>())
    , m_enableRustSession(enableRustSession)
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
    {
        m_rustSession = windirstat_rust_scan_session_create();
        if (m_rustSession == nullptr)
            throw std::runtime_error("Rust scan session allocation failed.");
    }
#endif
    SyncFromSnapshot(Snapshot());
}

ScanSessionState::~ScanSessionState()
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
        windirstat_rust_scan_session_destroy(static_cast<RustScanSession*>(m_rustSession));
#endif
}

void ScanSessionState::Start(const std::wstring& rootPath, ScanSessionOptions options)
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
    {
    const bool success = windirstat_rust_scan_session_start(
        static_cast<RustScanSession*>(m_rustSession),
        reinterpret_cast<const std::uint16_t*>(rootPath.data()),
        rootPath.size(),
        ToRustOptions(options));
    if (!success)
        throw std::runtime_error("Rust scan session failed to start.");
    }
    else
#endif
    {
    m_nativeSession->Start(rootPath, options);
    }
    SyncFromSnapshot(Snapshot());
}

void ScanSessionState::Reset()
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
        windirstat_rust_scan_session_reset(static_cast<RustScanSession*>(m_rustSession));
    else
#endif
    m_nativeSession->Reset();
    SyncFromSnapshot(Snapshot());
}

void ScanSessionState::ClearPendingWork()
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
        windirstat_rust_scan_session_clear_pending_work(static_cast<RustScanSession*>(m_rustSession));
    else
#endif
    m_nativeSession->ClearPendingWork();
    SyncFromSnapshot(Snapshot());
}

bool ScanSessionState::AddExternalPath(const std::wstring& path)
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    const bool added = m_enableRustSession
        ? windirstat_rust_scan_session_add_external_path(
            static_cast<RustScanSession*>(m_rustSession),
            reinterpret_cast<const std::uint16_t*>(path.data()),
            path.size())
        : m_nativeSession->AddExternalPath(path);
#else
    const bool added = m_nativeSession->AddExternalPath(path);
#endif
    SyncFromSnapshot(Snapshot());
    return added;
}

void ScanSessionState::CloseInput()
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
    {
        if (!windirstat_rust_scan_session_close_input(static_cast<RustScanSession*>(m_rustSession)))
            throw std::runtime_error("Rust scan session failed to close input.");
    }
    else
#endif
    m_nativeSession->CloseInput();
    SyncFromSnapshot(Snapshot());
}

void ScanSessionState::RequestCancel(const ScanTerminalReason reason)
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
    {
        if (!windirstat_rust_scan_session_request_cancel(
                static_cast<RustScanSession*>(m_rustSession),
                ToRustReason(reason)))
            throw std::runtime_error("Rust scan session failed to request cancellation.");
    }
    else
#endif
    m_nativeSession->RequestCancel(reason);
    SyncFromSnapshot(Snapshot());
}

ScanSessionAction ScanSessionState::NextAction()
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    ScanSessionAction action{};
    if (m_enableRustSession)
    {
        ScanSessionDirectoryResult directoryResult{};
        RustScanSessionActionView view{};
        const bool success = windirstat_rust_scan_session_next_action(
            static_cast<RustScanSession*>(m_rustSession),
            &view,
            &CollectRustDiscoveryEntry,
            &directoryResult);
        if (!success)
            throw std::runtime_error("Rust scan session failed to produce next action.");

        action = FromRustActionView(view);
        action.directoryResult = std::move(directoryResult);
        if (action.directoryResult.directoryPath.empty())
            action.directoryResult.directoryPath = action.path;
    }
    else
    {
        action = m_nativeSession->NextAction();
    }
#else
    ScanSessionAction action = m_nativeSession->NextAction();
#endif
    SyncFromSnapshot(Snapshot());
    return action;
}

std::optional<std::wstring> ScanSessionState::TryTakeNextPath()
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
        throw std::logic_error("TryTakeNextPath is fallback-only. Rust-backed callers must use NextAction().");
#endif
    return m_nativeSession->TryTakeNextPath();
}

ScanSessionDirectoryReport ScanSessionState::ReportDirectoryResult(
    const std::vector<TraversalChildCandidate>& childCandidates)
{
    ScanSessionDirectoryReport report{};
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
    {
        std::vector<RustScanSessionChildCandidateView> candidates;
        candidates.reserve(childCandidates.size());
        for (const auto& candidate : childCandidates)
        {
            RustScanSessionChildCandidateView view{};
            view.fullPath = reinterpret_cast<const std::uint16_t*>(candidate.fullPath.data());
            view.fullPathLen = candidate.fullPath.size();
            view.reparseTag = candidate.reparseTag;
            view.isProtectedReparsePoint = candidate.isProtectedReparsePoint ? 1 : 0;
            candidates.push_back(view);
        }

        const bool success = windirstat_rust_scan_session_report_directory_result(
            static_cast<RustScanSession*>(m_rustSession),
            candidates.empty() ? nullptr : candidates.data(),
            candidates.size(),
            &CollectRustSessionPath,
            &report.scheduledChildPaths);
        if (!success)
            throw std::runtime_error("Rust scan session failed to report directory result.");
    }
    else
#endif
    {
        report = m_nativeSession->ReportDirectoryResult(childCandidates);
    }
    SyncFromSnapshot(Snapshot());
    return report;
}

void ScanSessionState::ReportDirectoryFailure(ScanSessionFailure failure)
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
    {
        const bool success = windirstat_rust_scan_session_report_directory_failure(
            static_cast<RustScanSession*>(m_rustSession),
            ToRustFailureView(failure));
        if (!success)
            throw std::runtime_error("Rust scan session failed to report directory failure.");
    }
    else
#endif
    m_nativeSession->ReportDirectoryFailure(std::move(failure));
    SyncFromSnapshot(Snapshot());
}

bool ScanSessionState::IsInputClosed() const
{
    return m_inputClosed;
}

bool ScanSessionState::IsCancellationRequested() const
{
    return m_cancelRequested;
}

bool ScanSessionState::HasTerminalState() const
{
    return m_terminalReached;
}

bool ScanSessionState::ShouldIgnoreLateEvent() const
{
    return m_terminalReached;
}

bool ScanSessionState::IsDone() const
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
        return windirstat_rust_scan_session_is_done(static_cast<const RustScanSession*>(m_rustSession));
#endif
    return m_nativeSession->IsDone();
}

std::size_t ScanSessionState::PendingWorkCount() const
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
        return windirstat_rust_scan_session_pending_count(static_cast<const RustScanSession*>(m_rustSession));
#endif
    return m_nativeSession->PendingWorkCount();
}

std::uint64_t ScanSessionState::OutstandingWorkCount() const
{
    return m_outstandingWork;
}

ScanTerminalReason ScanSessionState::CancelReason() const
{
    return m_cancelReason;
}

ScanSessionTimingStats ScanSessionState::TimingStats() const
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
    {
        RustScanSessionTimingStatsView view{};
        const bool success = windirstat_rust_scan_session_timing_stats(
            static_cast<const RustScanSession*>(m_rustSession),
            &view);
        if (!success)
            throw std::runtime_error("Rust scan session timing stats failed.");
        return FromRustTimingStatsView(view);
    }
#endif
    return {};
}

std::optional<ScanSessionTerminalDecision> ScanSessionState::TryTakeTerminalDecision()
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
        throw std::logic_error("TryTakeTerminalDecision is fallback-only. Rust-backed callers must use NextAction().");
#endif
    const auto decision = m_nativeSession->TakeTerminalDecision();
    if (!decision.has_value())
    {
        SyncFromSnapshot(Snapshot());
        return std::nullopt;
    }
    SyncFromSnapshot(Snapshot());
    return decision.value();
}

bool ScanSessionState::UsesRustSession() const
{
    return m_enableRustSession;
}

ScanLifecycleSnapshot ScanSessionState::Snapshot() const
{
#if defined(_M_X64) && !defined(WINDIRSTAT_SCAN_HOST_SEAM_TESTS)
    if (m_enableRustSession)
    {
        RustScanSessionSnapshotView view{};
        const bool success = windirstat_rust_scan_session_snapshot(
            static_cast<const RustScanSession*>(m_rustSession),
            &view);
        if (!success)
            throw std::runtime_error("Rust scan session snapshot failed.");
        return FromRustSnapshotView(view);
    }
#endif
    return m_nativeSession->Snapshot();
}

void ScanSessionState::SyncFromSnapshot(const ScanLifecycleSnapshot& snapshot)
{
    m_outstandingWork = snapshot.outstandingWork;
    m_inputClosed = snapshot.inputClosed;
    m_cancelRequested = snapshot.cancelRequested;
    m_terminalReached = snapshot.terminalReached;
    m_terminalDecisionTaken = snapshot.terminalDecisionTaken;
    m_cancelReason = snapshot.cancelReason;
    m_terminalDecision = snapshot.terminalDecision;
}
