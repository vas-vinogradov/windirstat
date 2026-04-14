#include "pch.h"
#include "RemoteStub/RemoteScanHost.h"

#include "LegacyDiscoveryEngine.h"
#include "RemoteStub/BasicReplacementDiscoveryEngine.h"
#include "RemoteStub/ScanHostLogger.h"
#include "RemoteStub/NamedPipeRpcServer.h"
#include "RemoteStub/RustReplacementDiscoveryEngine.h"
#include "RemoteStub/StubDirectoryDiscoveryEngine.h"

namespace
{
void RpcHostLog(
    const std::uint64_t requestId,
    const std::wstring_view event,
    const std::wstring_view details = {},
    const ScanHostLogger::Level level = ScanHostLogger::Level::Milestone)
{
    std::wstring line = std::format(L"[RPC][HOST] request={} event={}",
        requestId, event);
    if (!details.empty())
        line += std::format(L" {}", details);

    ScanHostLogger::Log(line, level);
}

enum class DiscoveryEngineMode
{
    Legacy,
    Stub,
    BasicReplacement,
    Rust
};

std::wstring GetDiscoveryEngineModeValue()
{
    constexpr std::wstring_view variableName = L"WINDIRSTAT_REMOTE_DISCOVERY_ENGINE";
    const DWORD length = GetEnvironmentVariableW(variableName.data(), nullptr, 0);
    if (length == 0)
        return {};

    std::wstring value(length, L'\0');
    const DWORD actualLength = GetEnvironmentVariableW(variableName.data(), value.data(), length);
    if (actualLength == 0 || actualLength >= length)
        return {};

    value.resize(actualLength);
    _wcslwr_s(value.data(), value.size() + 1);
    return value;
}

constexpr bool IsRustDiscoveryBuildSupported()
{
#if defined(_M_X64)
    return true;
#else
    return false;
#endif
}

DiscoveryEngineMode GetDiscoveryEngineModeSelection()
{
    const std::wstring value = GetDiscoveryEngineModeValue();
    if (value == L"stub")
        return DiscoveryEngineMode::Stub;

    if (value == L"basic-replacement")
        return DiscoveryEngineMode::BasicReplacement;
    // Temporary: legacy remains an explicit migration fallback while Rust
    // becomes the preferred real immediate-child discovery implementation.
    if (value == L"legacy")
        return DiscoveryEngineMode::Legacy;
    if (value == L"rust")
        return IsRustDiscoveryBuildSupported() ? DiscoveryEngineMode::Rust : DiscoveryEngineMode::Legacy;

    // Contract: the default host path prefers Rust discovery when the build can
    // link the Rust FFI. Discovery remains a one-directory, immediate-child
    // seam; traversal and scheduling stay in RemoteScanHost.
    return IsRustDiscoveryBuildSupported() ? DiscoveryEngineMode::Rust : DiscoveryEngineMode::Legacy;
}

std::wstring_view GetDiscoveryEngineModeName(const DiscoveryEngineMode mode)
{
    switch (mode)
    {
    case DiscoveryEngineMode::Stub:
        return L"stub";
    case DiscoveryEngineMode::BasicReplacement:
        return L"basic-replacement";
    case DiscoveryEngineMode::Rust:
        return L"rust";
    case DiscoveryEngineMode::Legacy:
    default:
        return L"legacy";
    }
}

bool ShouldFollowReparseTagForHostTraversal(
    const DWORD reparseTag,
    const bool followMountPoints,
    const bool followSymbolicLinks,
    const bool followJunctions)
{
    if (reparseTag == 0)
        return true;

    if (reparseTag == IO_REPARSE_TAG_MOUNT_POINT)
        return followMountPoints;
    if (reparseTag == IO_REPARSE_TAG_SYMLINK)
        return followSymbolicLinks;
    if (reparseTag == IO_REPARSE_TAG_JUNCTION_POINT)
        return followJunctions;

    return true;
}
}

RemoteScanHost::RemoteScanHost()
    : m_discoveryEngine(CreateDiscoveryEngine())
{
    RpcHostLog(0, L"DiscoveryEngineSelected",
        std::format(L"implementation={}", GetDiscoveryEngineMode()));
}

int RemoteScanHost::Run(const std::wstring& pipeName)
{
    RpcHostLog(0, L"HostRunStarted", std::format(L"pipe=\"{}\"", pipeName));
    RpcHostLog(0, L"ServerConstructed", std::format(L"pipe=\"{}\"", pipeName));
    NamedPipeRpcServer server(pipeName);
    RpcHostLog(0, L"HostListenStarting", std::format(L"pipe=\"{}\"", pipeName));
    if (!server.Listen())
    {
        RpcHostLog(0, L"HostListenFailed", std::format(L"pipe=\"{}\" error={}",
            pipeName, GetLastError()));
        return 2;
    }

    RpcHostLog(0, L"HostListenConnected", std::format(L"pipe=\"{}\"", pipeName));

    ActiveRequestState activeRequest{};
    std::optional<RpcStartScanRequest> queuedStart;

    while (true)
    {
        if (activeRequest.requestId != 0 && server.HasPendingRequestMessage())
        {
            const auto messageOpt = server.ReadRequestMessage();
            if (!messageOpt.has_value())
                break;

            HandleRequestMessage(activeRequest, queuedStart, messageOpt.value());
            continue;
        }

        if (activeRequest.requestId != 0 && activeRequest.cancelPending)
        {
            ASSERT(activeRequest.requestId != 0);
            ASSERT(activeRequest.cancelPending);
            ASSERT(activeRequest.pendingPaths.empty());
            RpcHostLog(activeRequest.requestId, L"TerminalEmitted",
                std::format(L"terminal=Canceled reason={} queueSize={}",
                    static_cast<int>(activeRequest.cancelReason), activeRequest.pendingPaths.size()));
            if (!SendCanceled(server, activeRequest))
                return 3;

            ResetActiveRequest(activeRequest);
            PromoteQueuedStart(activeRequest, queuedStart);
            continue;
        }

        if (activeRequest.requestId != 0 && activeRequest.inputClosed && activeRequest.pendingPaths.empty())
        {
            ASSERT(activeRequest.requestId != 0);
            ASSERT(activeRequest.inputClosed);
            ASSERT(activeRequest.pendingPaths.empty());
            ASSERT(!activeRequest.cancelPending);
            RpcHostLog(activeRequest.requestId, L"TerminalEmitted", L"terminal=Completed queueSize=0");
            if (!SendCompleted(server, activeRequest))
                return 3;

            ResetActiveRequest(activeRequest);
            PromoteQueuedStart(activeRequest, queuedStart);
            continue;
        }

        if (!activeRequest.pendingPaths.empty())
        {
            ASSERT(activeRequest.requestId != 0);
            const std::wstring path = std::move(activeRequest.pendingPaths.front());
            activeRequest.pendingPaths.pop_front();
            RpcHostLog(activeRequest.requestId, L"DirectoryProcessingStarted",
                std::format(L"path=\"{}\" queueSize={}", path, activeRequest.pendingPaths.size()),
                ScanHostLogger::Level::Verbose);

            switch (ExecuteDiscovery(server, activeRequest, path))
            {
            case DiscoveryResult::ProgressSent:
                RpcHostLog(activeRequest.requestId, L"DirectoryProcessingFinished",
                    std::format(L"path=\"{}\" result=ProgressSent queueSize={}", path, activeRequest.pendingPaths.size()),
                    ScanHostLogger::Level::Verbose);
                break;
            case DiscoveryResult::RequestFailed:
                RpcHostLog(activeRequest.requestId, L"DirectoryProcessingFinished",
                    std::format(L"path=\"{}\" result=Failed queueSize={}", path, activeRequest.pendingPaths.size()));
                ResetActiveRequest(activeRequest);
                PromoteQueuedStart(activeRequest, queuedStart);
                break;
            case DiscoveryResult::TransportFailure:
                RpcHostLog(activeRequest.requestId, L"DirectoryProcessingFinished",
                    std::format(L"path=\"{}\" result=TransportFailure queueSize={}", path, activeRequest.pendingPaths.size()));
                return 3;
            }

            continue;
        }

        const auto messageOpt = server.ReadRequestMessage();
        if (!messageOpt.has_value())
            break;

        HandleRequestMessage(activeRequest, queuedStart, messageOpt.value());
    }

    return 0;
}

void RemoteScanHost::HandleRequestMessage(
    ActiveRequestState& activeRequest,
    std::optional<RpcStartScanRequest>& queuedStart,
    const RpcRequestMessage& message)
{
    std::visit([&](const auto& typedMessage)
    {
        using TMessage = std::decay_t<decltype(typedMessage)>;

        if constexpr (std::is_same_v<TMessage, RpcStartScanRequest>)
        {
            RpcHostLog(typedMessage.requestId, L"RequestReceived",
                std::format(L"type=StartScan path=\"{}\"", typedMessage.rootPath),
                ScanHostLogger::Level::Verbose);
            if (activeRequest.requestId == 0)
            {
                ASSERT(typedMessage.requestId != 0);
                RpcHostLog(typedMessage.requestId, L"StartScanReceived",
                    std::format(L"path=\"{}\" followMountPoints={} followSymbolicLinks={} followJunctions={}",
                        typedMessage.rootPath,
                        typedMessage.followMountPoints ? 1 : 0,
                        typedMessage.followSymbolicLinks ? 1 : 0,
                        typedMessage.followJunctions ? 1 : 0));
                StartActiveRequest(activeRequest, typedMessage);
            }
            else
            {
                ASSERT(activeRequest.requestId != 0);
                ASSERT(typedMessage.requestId != 0);
                ASSERT(activeRequest.requestId != typedMessage.requestId);
                RpcHostLog(activeRequest.requestId, L"ReplacementTriggered",
                    std::format(L"replacement={} path=\"{}\"", typedMessage.requestId, typedMessage.rootPath));
                queuedStart = typedMessage;
                activeRequest.cancelPending = true;
                activeRequest.cancelReason = ScanTerminalReason::Restarted;
                activeRequest.inputClosed = true;
                activeRequest.pendingPaths.clear();
                ASSERT(activeRequest.cancelPending);
                ASSERT(activeRequest.inputClosed);
                ASSERT(activeRequest.pendingPaths.empty());
            }
        }

        if constexpr (std::is_same_v<TMessage, RpcEnqueueRequest>)
        {
            RpcHostLog(typedMessage.requestId, L"RequestReceived",
                std::format(L"type=Enqueue path=\"{}\"", typedMessage.rootPath),
                ScanHostLogger::Level::Verbose);
            if (typedMessage.requestId == activeRequest.requestId && !activeRequest.inputClosed && !activeRequest.cancelPending)
            {
                ASSERT(activeRequest.requestId != 0);
                RpcHostLog(typedMessage.requestId, L"EnqueueReceived",
                    std::format(L"path=\"{}\" queueSize={}", typedMessage.rootPath, activeRequest.pendingPaths.size() + 1),
                    ScanHostLogger::Level::Verbose);
                activeRequest.pendingPaths.push_back(typedMessage.rootPath);
            }
        }

        if constexpr (std::is_same_v<TMessage, RpcCloseRequestInput>)
        {
            RpcHostLog(typedMessage.requestId, L"RequestReceived", L"type=CloseRequestInput", ScanHostLogger::Level::Verbose);
            if (typedMessage.requestId == activeRequest.requestId && !activeRequest.cancelPending)
            {
                ASSERT(activeRequest.requestId != 0);
                RpcHostLog(typedMessage.requestId, L"CloseRequestInputReceived",
                    std::format(L"queueSize={}", activeRequest.pendingPaths.size()));
                activeRequest.inputClosed = true;
            }
        }

        if constexpr (std::is_same_v<TMessage, RpcCancelScanRequest>)
        {
            RpcHostLog(typedMessage.requestId, L"RequestReceived",
                std::format(L"type=CancelScan reason={}", static_cast<int>(typedMessage.reason)),
                ScanHostLogger::Level::Verbose);
            if (typedMessage.requestId == activeRequest.requestId)
            {
                ASSERT(activeRequest.requestId != 0);
                RpcHostLog(typedMessage.requestId, L"CancelReceived",
                    std::format(L"reason={} queueSize={}", static_cast<int>(typedMessage.reason), activeRequest.pendingPaths.size()));
                activeRequest.cancelPending = true;
                activeRequest.cancelReason = typedMessage.reason;
                activeRequest.inputClosed = true;
                activeRequest.pendingPaths.clear();
                queuedStart.reset();
                ASSERT(activeRequest.pendingPaths.empty());
            }
        }
    }, message);
}

void RemoteScanHost::ResetActiveRequest(ActiveRequestState& state)
{
    const std::uint64_t requestId = state.requestId;
    ASSERT(requestId != 0);
    state.requestId = 0;
    state.pendingPaths.clear();
    state.inputClosed = false;
    state.cancelPending = false;
    state.followMountPoints = false;
    state.followSymbolicLinks = false;
    state.followJunctions = false;
    state.cancelReason = ScanTerminalReason::EngineInterrupted;
    ASSERT(state.requestId == 0);
    ASSERT(state.pendingPaths.empty());
    ASSERT(!state.inputClosed);
    ASSERT(!state.cancelPending);
    RpcHostLog(requestId, L"RequestReset", L"queueSize=0 inputClosed=0 cancelPending=0");
}

void RemoteScanHost::StartActiveRequest(ActiveRequestState& state, const RpcStartScanRequest& request)
{
    const std::uint64_t requestId = request.requestId;
    const std::wstring& rootPath = request.rootPath;
    ASSERT(requestId != 0);
    ASSERT(state.requestId == 0);
    ASSERT(state.pendingPaths.empty());
    ASSERT(!state.inputClosed);
    ASSERT(!state.cancelPending);
    state.requestId = requestId;
    state.pendingPaths.clear();
    state.pendingPaths.push_back(rootPath);
    state.inputClosed = false;
    state.cancelPending = false;
    state.followMountPoints = request.followMountPoints;
    state.followSymbolicLinks = request.followSymbolicLinks;
    state.followJunctions = request.followJunctions;
    state.cancelReason = ScanTerminalReason::EngineInterrupted;
    RpcHostLog(requestId, L"RequestActivated",
        std::format(L"path=\"{}\" queueSize=1 inputClosed=0 cancelPending=0 followMountPoints={} followSymbolicLinks={} followJunctions={}",
            rootPath,
            state.followMountPoints ? 1 : 0,
            state.followSymbolicLinks ? 1 : 0,
            state.followJunctions ? 1 : 0));
}

void RemoteScanHost::PromoteQueuedStart(ActiveRequestState& state, std::optional<RpcStartScanRequest>& queuedStart)
{
    if (!queuedStart.has_value())
        return;

    ASSERT(state.requestId == 0);
    ASSERT(state.pendingPaths.empty());
    ASSERT(!state.inputClosed);
    ASSERT(!state.cancelPending);
    const RpcStartScanRequest nextStart = std::move(queuedStart.value());
    queuedStart.reset();
    ASSERT(nextStart.requestId != 0);
    RpcHostLog(nextStart.requestId, L"ReplacementPromotion",
        std::format(L"path=\"{}\"", nextStart.rootPath));
    StartActiveRequest(state, nextStart);
}

bool RemoteScanHost::SendCanceled(NamedPipeRpcServer& server, const ActiveRequestState& state)
{
    ASSERT(state.requestId != 0);
    ASSERT(state.cancelPending);
    ASSERT(state.pendingPaths.empty());
    RpcScanCanceledEvent canceledEvent{};
    canceledEvent.requestId = state.requestId;
    canceledEvent.reason = state.cancelReason;
    return server.SendEventMessage(RpcEventMessage(canceledEvent));
}

bool RemoteScanHost::SendCompleted(NamedPipeRpcServer& server, const ActiveRequestState& state)
{
    ASSERT(state.requestId != 0);
    ASSERT(state.inputClosed);
    ASSERT(state.pendingPaths.empty());
    ASSERT(!state.cancelPending);
    RpcScanCompletedEvent completed{};
    completed.requestId = state.requestId;
    return server.SendEventMessage(RpcEventMessage(completed));
}

RemoteScanHost::DiscoveryResult RemoteScanHost::ExecuteDiscovery(NamedPipeRpcServer& server, ActiveRequestState& state, const std::wstring& path)
{
    const std::uint64_t requestId = state.requestId;
    ASSERT(requestId != 0);
    try
    {
        RpcHostLog(requestId, L"EngineInvocationStarted", std::format(L"path=\"{}\"", path));
        DirectoryDiscoveryRequest request{};
        request.path = path;

        ASSERT(m_discoveryEngine != nullptr);
        DiscoveryBatch discoveryBatch = m_discoveryEngine->Discover(request);
        std::size_t queuedChildren = 0;
        for (const auto& directory : discoveryBatch.directories)
        {
            if (!ShouldQueueDiscoveredDirectory(state, directory))
                continue;

            state.pendingPaths.push_back(directory.fullPath);
            ++queuedChildren;
        }

        if (queuedChildren != 0)
        {
            RpcHostLog(requestId, L"TraversalQueuedChildren",
                std::format(L"path=\"{}\" queuedChildren={} queueSize={}",
                    path, queuedChildren, state.pendingPaths.size()),
                ScanHostLogger::Level::Verbose);
        }

        RpcDirectoryProgressEvent progress{};
        progress.requestId = requestId;
        progress.directoryPath = path;
        progress.files = std::move(discoveryBatch.files);
        progress.directories = std::move(discoveryBatch.directories);
        progress.finished = true;
        RpcHostLog(requestId, L"DirectoryProgressEmitted",
            std::format(L"path=\"{}\" files={} directories={} finished=1",
                path, progress.files.size(), progress.directories.size()),
            ScanHostLogger::Level::Verbose);
        return server.SendEventMessage(RpcEventMessage(std::move(progress)))
            ? DiscoveryResult::ProgressSent
            : DiscoveryResult::TransportFailure;
    }
    catch (const std::exception& ex)
    {
        RpcScanFailedEvent failed{};
        failed.requestId = requestId;
        failed.path = path;
        failed.message = std::wstring(CA2W(ex.what()));
        failed.errorCode = 0;
        RpcHostLog(requestId, L"TerminalEmitted",
            std::format(L"terminal=Failed path=\"{}\" errorCode={} message=\"{}\"",
                failed.path, failed.errorCode, failed.message));
        return server.SendEventMessage(RpcEventMessage(std::move(failed)))
            ? DiscoveryResult::RequestFailed
            : DiscoveryResult::TransportFailure;
    }
}

bool RemoteScanHost::ShouldQueueDiscoveredDirectory(const ActiveRequestState& state, const DiscoveredDirectory& directory)
{
    if (directory.isProtectedReparsePoint)
        return false;

    return ShouldFollowReparseTagForHostTraversal(
        directory.reparseTag,
        state.followMountPoints,
        state.followSymbolicLinks,
        state.followJunctions);
}

std::unique_ptr<IDirectoryDiscoveryEngine> RemoteScanHost::CreateDiscoveryEngine()
{
    switch (GetDiscoveryEngineModeSelection())
    {
    case DiscoveryEngineMode::Stub:
        return std::make_unique<StubDirectoryDiscoveryEngine>();
    case DiscoveryEngineMode::BasicReplacement:
        return std::make_unique<BasicReplacementDiscoveryEngine>();
    case DiscoveryEngineMode::Rust:
        return std::make_unique<RustReplacementDiscoveryEngine>();
    case DiscoveryEngineMode::Legacy:
    default:
        break;
    }

    return std::make_unique<LegacyDiscoveryEngine>();
}

std::wstring RemoteScanHost::GetDiscoveryEngineMode()
{
    return std::wstring(GetDiscoveryEngineModeName(GetDiscoveryEngineModeSelection()));
}
