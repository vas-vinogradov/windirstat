#pragma once

#include "DiscoveredDirectory.h"
#include "DiscoveredFile.h"
#include "RemoteStub/ScanLifecycleContract.h"
#include "RemoteStub/ScanTraversalContract.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using ScanSessionFailure = ScanLifecycleFailure;
using ScanSessionTerminalKind = ScanLifecycleTerminalKind;
using ScanSessionTerminalDecision = ScanLifecycleTerminalDecision;

struct ScanSessionOptions
{
    bool followMountPoints = false;
    bool followSymbolicLinks = false;
    bool followJunctions = false;
};

struct ScanSessionDirectoryReport
{
    std::vector<std::wstring> scheduledChildPaths;
};

struct ScanSessionDirectoryResult
{
    // Contract: Rust provides a fully populated discovery batch that the host
    // can forward directly into RpcDirectoryProgressEvent without additional
    // enrichment on the Rust-backed path.
    std::wstring directoryPath;
    std::vector<DiscoveredFile> files;
    std::vector<DiscoveredDirectory> directories;
    bool finished = true;
    std::size_t scheduledChildCount = 0;
};

struct ScanSessionTimingStats
{
    std::uint64_t discoveryWorkerElapsedNs = 0;
    std::uint64_t schedulingElapsedNs = 0;
    std::uint64_t noEventPollCount = 0;
    std::size_t workerCount = 0;
};

enum class ScanSessionActionKind
{
    RequestDirectory,
    EmitDirectoryResult,
    WaitingForInput,
    Complete,
    Cancelled,
    Failed
};

struct ScanSessionAction
{
    ScanSessionActionKind kind = ScanSessionActionKind::WaitingForInput;
    std::wstring path;
    ScanSessionDirectoryResult directoryResult{};
    ScanTerminalReason cancelReason = ScanTerminalReason::EngineInterrupted;
    ScanSessionFailure failure{};
};

// Intent: host-side adapter for the Rust-owned scan session.
// Contract: Rust owns progression and lifecycle decisions. On Rust-backed
// sessions it also owns immediate-child discovery invocation and returns
// emit-ready directory results for the host to send over RPC. The C++ host
// remains responsible for transport, observation, and compatibility fallback.
// Temporary: non-x64/test builds keep a native fallback so unsupported host
// targets and seam tests remain runnable until every target links the Rust
// session API.
// Removal condition: remove the native fallback once the Rust engine session is
// available for all scan-host builds.
class ScanSessionState
{
public:
    explicit ScanSessionState(bool enableRustSession = true);
    ~ScanSessionState();

    ScanSessionState(const ScanSessionState&) = delete;
    ScanSessionState& operator=(const ScanSessionState&) = delete;

    void Start(const std::wstring& rootPath, ScanSessionOptions options);
    void Reset();
    void ClearPendingWork();

    // Contract: these methods are no-ops after a terminal decision so host
    // message races cannot emit a second terminal event for the same request.
    bool AddExternalPath(const std::wstring& path);
    void CloseInput();
    void RequestCancel(ScanTerminalReason reason);

    // Contract: this is the authoritative session-driving API for both the
    // Rust-backed engine path and the native fallback path.
    ScanSessionAction NextAction();

    // Temporary: compatibility shim for fallback/native-only tests and code.
    // Rust-backed callers must drive the session through NextAction() instead.
    std::optional<std::wstring> TryTakeNextPath();

    ScanSessionDirectoryReport ReportDirectoryResult(const std::vector<TraversalChildCandidate>& childCandidates);
    void ReportDirectoryFailure(ScanSessionFailure failure);

    bool IsInputClosed() const;
    bool IsCancellationRequested() const;
    bool HasTerminalState() const;
    bool ShouldIgnoreLateEvent() const;
    bool IsDone() const;
    std::size_t PendingWorkCount() const;
    std::uint64_t OutstandingWorkCount() const;
    ScanTerminalReason CancelReason() const;
    ScanSessionTimingStats TimingStats() const;

    // Temporary: compatibility shim for fallback/native-only tests and code.
    // Rust-backed callers must consume terminal states from NextAction().
    std::optional<ScanSessionTerminalDecision> TryTakeTerminalDecision();
    bool UsesRustSession() const;

private:
    class NativeSessionState;

    ScanLifecycleSnapshot Snapshot() const;
    void SyncFromSnapshot(const ScanLifecycleSnapshot& snapshot);

    std::uint64_t m_outstandingWork = 0;
    bool m_inputClosed = false;
    bool m_cancelRequested = false;
    bool m_terminalReached = false;
    bool m_terminalDecisionTaken = false;
    ScanTerminalReason m_cancelReason = ScanTerminalReason::EngineInterrupted;
    std::optional<ScanSessionTerminalDecision> m_terminalDecision;
    std::unique_ptr<NativeSessionState> m_nativeSession;
    bool m_enableRustSession = true;
    void* m_rustSession = nullptr;
};
