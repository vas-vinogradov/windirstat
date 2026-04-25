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
std::uint64_t ReadPerformanceCounter()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

double PerformanceTicksToMilliseconds(const std::uint64_t ticks)
{
    static const double frequency = []()
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();

    return (static_cast<double>(ticks) * 1000.0) / frequency;
}

bool IsTimingEnabled()
{
    constexpr std::wstring_view variableName = L"WINDIRSTAT_SCAN_TIMING";
    std::array<wchar_t, 16> value{};
    const DWORD length = GetEnvironmentVariableW(variableName.data(), value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
        return false;

    std::wstring text(value.data(), length);
    _wcslwr_s(text.data(), text.size() + 1);
    return text == L"1" || text == L"true" || text == L"yes";
}

bool IsBatchProgressEnabled()
{
    constexpr std::wstring_view variableName = L"WINDIRSTAT_RPC_BATCH_PROGRESS";
    std::array<wchar_t, 16> value{};
    const DWORD length = GetEnvironmentVariableW(variableName.data(), value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
        return false;

    std::wstring text(value.data(), length);
    _wcslwr_s(text.data(), text.size() + 1);
    return text == L"1" || text == L"true" || text == L"yes";
}

std::size_t GetBatchProgressSize()
{
    constexpr std::wstring_view variableName = L"WINDIRSTAT_RPC_BATCH_SIZE";
    std::array<wchar_t, 32> value{};
    const DWORD length = GetEnvironmentVariableW(variableName.data(), value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
        return 64;

    try
    {
        return std::clamp<std::size_t>(std::stoull(std::wstring(value.data(), length)), 1, 512);
    }
    catch (...)
    {
        return 64;
    }
}

std::uint64_t MillisecondsToPerformanceTicks(const std::uint64_t milliseconds)
{
    static const std::uint64_t frequency = []()
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<std::uint64_t>(value.QuadPart);
    }();

    return (frequency * milliseconds) / 1000;
}

std::uint64_t GetBatchProgressMaxDelayTicks()
{
    constexpr std::wstring_view variableName = L"WINDIRSTAT_RPC_BATCH_MAX_DELAY_MS";
    std::array<wchar_t, 32> value{};
    const DWORD length = GetEnvironmentVariableW(variableName.data(), value.data(), static_cast<DWORD>(value.size()));
    std::uint64_t milliseconds = 10;
    if (length != 0 && length < value.size())
    {
        try
        {
            milliseconds = std::clamp<std::uint64_t>(std::stoull(std::wstring(value.data(), length)), 0, 250);
        }
        catch (...)
        {
            milliseconds = 10;
        }
    }

    return MillisecondsToPerformanceTicks(milliseconds);
}

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

    // Contract: the default host path prefers the Rust-owned scan engine when
    // the build can link the Rust FFI. C++ remains the RPC adapter shell;
    // fallback discovery modes are explicit compatibility choices.
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

}

RemoteScanHost::RemoteScanHost()
    : m_rustSessionOwnsDiscovery(GetDiscoveryEngineModeSelection() == DiscoveryEngineMode::Rust)
{
    // Temporary: fallback discovery remains host-owned only when an explicit
    // compatibility mode is selected. Rust-backed sessions discover internally
    // and hand C++ emit-ready progress payloads through NextAction().
    if (!m_rustSessionOwnsDiscovery)
        m_directoryProcessor = std::make_unique<DiscoveryDirectoryWorkItemProcessor>(CreateDiscoveryEngine());

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

    ActiveRequestState activeRequest{ m_rustSessionOwnsDiscovery };
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

        if (activeRequest.requestId != 0)
        {
            const std::uint64_t nextActionStart = ReadPerformanceCounter();
            const ScanSessionAction action = activeRequest.session.NextAction();
            activeRequest.timing.nextActionTicks += ReadPerformanceCounter() - nextActionStart;
            switch (action.kind)
            {
            case ScanSessionActionKind::EmitDirectoryResult:
            {
                if (EmitDirectoryResultAction(server, activeRequest, action.directoryResult) == DiscoveryResult::TransportFailure)
                    return 3;

                continue;
            }
            case ScanSessionActionKind::RequestDirectory:
            {
                ASSERT(activeRequest.requestId != 0);
                ASSERT(!activeRequest.session.UsesRustSession());
                const std::wstring& path = action.path;
                RpcHostLog(activeRequest.requestId, L"DirectoryProcessingStarted",
                    std::format(L"path=\"{}\" queueSize={}", path, activeRequest.session.PendingWorkCount()),
                    ScanHostLogger::Level::Verbose);

                DirectoryWorkItemResult workItemResult = ProcessDirectoryWorkItem(activeRequest, path);
                DirectoryWorkItemPolicyResult policyResult = ApplyDirectoryWorkItemPolicy(activeRequest, std::move(workItemResult));
                DirectoryWorkItemEffectsResult effectsResult = ApplyDirectoryWorkItemEffects(server, activeRequest, std::move(policyResult));
                switch (effectsResult.discoveryResult)
                {
                case DiscoveryResult::ProgressSent:
                    RpcHostLog(activeRequest.requestId, L"DirectoryProcessingFinished",
                        std::format(L"path=\"{}\" result=ProgressSent queueSize={}", path, activeRequest.session.PendingWorkCount()),
                        ScanHostLogger::Level::Verbose);
                    break;
                case DiscoveryResult::RequestFailed:
                    RpcHostLog(activeRequest.requestId, L"DirectoryProcessingFinished",
                        std::format(L"path=\"{}\" result=Failed queueSize={}", path, activeRequest.session.PendingWorkCount()));
                    break;
                case DiscoveryResult::TransportFailure:
                    RpcHostLog(activeRequest.requestId, L"DirectoryProcessingFinished",
                        std::format(L"path=\"{}\" result=TransportFailure queueSize={}", path, activeRequest.session.PendingWorkCount()));
                    return 3;
                }

                continue;
            }
            case ScanSessionActionKind::Complete:
            case ScanSessionActionKind::Cancelled:
            case ScanSessionActionKind::Failed:
            {
                if (!FlushDirectoryProgressBatch(server, activeRequest))
                    return 3;

                const TerminalEmissionResult terminalResult = EmitTerminalAction(server, activeRequest, action);
                if (terminalResult == TerminalEmissionResult::TransportFailure)
                    return 3;

                if (terminalResult == TerminalEmissionResult::Sent)
                {
                    if (!server.FlushEventMessages())
                        return 3;
                    EmitTimingSummary(server, activeRequest, action);
                    ResetActiveRequest(activeRequest);
                    PromoteQueuedStart(activeRequest, queuedStart);
                    continue;
                }
                break;
            }
            case ScanSessionActionKind::WaitingForInput:
                ++activeRequest.timing.waitingPollCount;
                if (ShouldFlushDirectoryProgressBatchByDelay(activeRequest) &&
                    !FlushDirectoryProgressBatch(server, activeRequest))
                {
                    return 3;
                }
                // Contract: for Rust-backed sessions this also means "no
                // completed engine event is ready yet." If work is still
                // outstanding, keep polling the Rust-owned engine instead of
                // blocking on the pipe and starving completed worker results.
                if (activeRequest.session.OutstandingWorkCount() != 0)
                {
                    Sleep(1);
                    continue;
                }
                if (!FlushDirectoryProgressBatch(server, activeRequest))
                    return 3;
                break;
            default:
                break;
            }
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
                activeRequest.session.ClearPendingWork();
                activeRequest.session.RequestCancel(ScanTerminalReason::Restarted);
                ASSERT(activeRequest.session.IsCancellationRequested());
                ASSERT(activeRequest.session.IsInputClosed());
                ASSERT(activeRequest.session.PendingWorkCount() == 0);
            }
        }

        if constexpr (std::is_same_v<TMessage, RpcEnqueueRequest>)
        {
            RpcHostLog(typedMessage.requestId, L"RequestReceived",
                std::format(L"type=Enqueue path=\"{}\"", typedMessage.rootPath),
                ScanHostLogger::Level::Verbose);
            if (typedMessage.requestId == activeRequest.requestId &&
                !activeRequest.session.IsInputClosed() &&
                !activeRequest.session.IsCancellationRequested() &&
                !activeRequest.session.ShouldIgnoreLateEvent())
            {
                ASSERT(activeRequest.requestId != 0);
                RpcHostLog(typedMessage.requestId, L"EnqueueReceived",
                    std::format(L"path=\"{}\" queueSize={}", typedMessage.rootPath, activeRequest.session.PendingWorkCount() + 1),
                    ScanHostLogger::Level::Verbose);
                if (!activeRequest.session.AddExternalPath(typedMessage.rootPath))
                {
                    RpcHostLog(typedMessage.requestId, L"EnqueueIgnored",
                        std::format(L"path=\"{}\" reason=AlreadyTracked queueSize={}",
                            typedMessage.rootPath, activeRequest.session.PendingWorkCount()),
                        ScanHostLogger::Level::Verbose);
                }
            }
        }

        if constexpr (std::is_same_v<TMessage, RpcCloseRequestInput>)
        {
            RpcHostLog(typedMessage.requestId, L"RequestReceived", L"type=CloseRequestInput", ScanHostLogger::Level::Verbose);
            if (typedMessage.requestId == activeRequest.requestId &&
                !activeRequest.session.IsCancellationRequested() &&
                !activeRequest.session.ShouldIgnoreLateEvent())
            {
                ASSERT(activeRequest.requestId != 0);
                RpcHostLog(typedMessage.requestId, L"CloseRequestInputReceived",
                    std::format(L"queueSize={}", activeRequest.session.PendingWorkCount()));
                activeRequest.session.CloseInput();
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
                    std::format(L"reason={} queueSize={}", static_cast<int>(typedMessage.reason), activeRequest.session.PendingWorkCount()));
                activeRequest.session.ClearPendingWork();
                activeRequest.session.RequestCancel(typedMessage.reason);
                queuedStart.reset();
                ASSERT(activeRequest.session.PendingWorkCount() == 0);
            }
        }
    }, message);
}

void RemoteScanHost::ResetActiveRequest(ActiveRequestState& state)
{
    const std::uint64_t requestId = state.requestId;
    ASSERT(requestId != 0);
    state.requestId = 0;
    state.timing = {};
    state.pendingProgressBatch.clear();
    state.pendingProgressBatchStartedTicks = 0;
    state.session.Reset();
    ASSERT(state.requestId == 0);
    ASSERT(state.session.PendingWorkCount() == 0);
    ASSERT(!state.session.IsInputClosed());
    ASSERT(!state.session.IsCancellationRequested());
    RpcHostLog(requestId, L"RequestReset", L"queueSize=0 inputClosed=0 cancelPending=0");
}

void RemoteScanHost::StartActiveRequest(ActiveRequestState& state, const RpcStartScanRequest& request)
{
    const std::uint64_t requestId = request.requestId;
    const std::wstring& rootPath = request.rootPath;
    ASSERT(requestId != 0);
    ASSERT(state.requestId == 0);
    ASSERT(state.session.PendingWorkCount() == 0);
    ASSERT(!state.session.IsInputClosed());
    ASSERT(!state.session.IsCancellationRequested());
    state.requestId = requestId;
    state.timing = {};
    state.timing.requestStartedTicks = ReadPerformanceCounter();
    state.batchProgressEnabled = IsBatchProgressEnabled();
    state.batchProgressSize = GetBatchProgressSize();
    state.batchProgressMaxDelayTicks = GetBatchProgressMaxDelayTicks();
    state.pendingProgressBatch.clear();
    state.pendingProgressBatch.reserve(state.batchProgressSize);
    state.pendingProgressBatchStartedTicks = 0;
    ScanSessionOptions options{};
    options.followMountPoints = request.followMountPoints;
    options.followSymbolicLinks = request.followSymbolicLinks;
    options.followJunctions = request.followJunctions;
    state.session.Start(rootPath, options);
    RpcHostLog(requestId, L"RequestActivated",
        std::format(L"path=\"{}\" queueSize=1 inputClosed=0 cancelPending=0 followMountPoints={} followSymbolicLinks={} followJunctions={}",
            rootPath,
            options.followMountPoints ? 1 : 0,
            options.followSymbolicLinks ? 1 : 0,
            options.followJunctions ? 1 : 0));
    if (state.batchProgressEnabled)
    {
        RpcHostLog(requestId, L"DirectoryProgressBatchingEnabled",
            std::format(L"batchSize={} maxDelayTicks={}", state.batchProgressSize, state.batchProgressMaxDelayTicks));
    }
}

void RemoteScanHost::PromoteQueuedStart(ActiveRequestState& state, std::optional<RpcStartScanRequest>& queuedStart)
{
    if (!queuedStart.has_value())
        return;

    ASSERT(state.requestId == 0);
    ASSERT(state.session.PendingWorkCount() == 0);
    ASSERT(!state.session.IsInputClosed());
    ASSERT(!state.session.IsCancellationRequested());
    const RpcStartScanRequest nextStart = std::move(queuedStart.value());
    queuedStart.reset();
    ASSERT(nextStart.requestId != 0);
    RpcHostLog(nextStart.requestId, L"ReplacementPromotion",
        std::format(L"path=\"{}\"", nextStart.rootPath));
    StartActiveRequest(state, nextStart);
}

RemoteScanHost::DiscoveryResult RemoteScanHost::EmitDirectoryResultAction(
    NamedPipeRpcServer& server,
    ActiveRequestState& state,
    const ScanSessionDirectoryResult& result)
{
    ASSERT(state.requestId != 0);
    ASSERT(result.finished);

    if (result.scheduledChildCount != 0)
    {
        RpcHostLog(state.requestId, L"TraversalQueuedChildren",
            std::format(L"path=\"{}\" queuedChildren={} queueSize={}",
                result.directoryPath, result.scheduledChildCount, state.session.PendingWorkCount()),
            ScanHostLogger::Level::Verbose);
    }

    const std::uint64_t conversionStart = ReadPerformanceCounter();
    RpcDirectoryProgressEvent progress{};
    progress.requestId = state.requestId;
    progress.directoryPath = result.directoryPath;
    progress.files = result.files;
    progress.directories = result.directories;
    progress.finished = result.finished;
    state.timing.rpcConversionTicks += ReadPerformanceCounter() - conversionStart;
    RpcHostLog(state.requestId, L"DirectoryProgressEmitted",
        std::format(L"path=\"{}\" files={} directories={} finished=1",
            progress.directoryPath, progress.files.size(), progress.directories.size()),
        ScanHostLogger::Level::Verbose);
    return SendOrBufferDirectoryProgress(server, state, std::move(progress));
}

RemoteScanHost::TerminalEmissionResult RemoteScanHost::EmitTerminalAction(
    NamedPipeRpcServer& server,
    ActiveRequestState& state,
    const ScanSessionAction& action)
{
    ASSERT(state.requestId != 0);
    switch (action.kind)
    {
    case ScanSessionActionKind::Complete:
        ASSERT(state.session.IsDone());
        RpcHostLog(state.requestId, L"TerminalEmitted", L"terminal=Completed queueSize=0");
        return SendCompleted(server, state)
            ? TerminalEmissionResult::Sent
            : TerminalEmissionResult::TransportFailure;
    case ScanSessionActionKind::Cancelled:
        ASSERT(state.session.IsDone());
        RpcHostLog(state.requestId, L"TerminalEmitted",
            std::format(L"terminal=Canceled reason={} queueSize={}",
                static_cast<int>(action.cancelReason), state.session.PendingWorkCount()));
        return SendCanceled(server, state, action.cancelReason)
            ? TerminalEmissionResult::Sent
            : TerminalEmissionResult::TransportFailure;
    case ScanSessionActionKind::Failed:
        RpcHostLog(state.requestId, L"TerminalEmitted",
            std::format(L"terminal=Failed path=\"{}\" errorCode={} message=\"{}\"",
                action.failure.path, action.failure.errorCode, action.failure.message));
        return SendFailed(server, state, action.failure)
            ? TerminalEmissionResult::Sent
            : TerminalEmissionResult::TransportFailure;
    default:
        ASSERT(false);
        return TerminalEmissionResult::None;
    }
}

bool RemoteScanHost::SendCanceled(NamedPipeRpcServer& server, ActiveRequestState& state, const ScanTerminalReason reason)
{
    const std::uint64_t conversionStart = ReadPerformanceCounter();
    RpcScanCanceledEvent canceledEvent{};
    canceledEvent.requestId = state.requestId;
    canceledEvent.reason = reason;
    state.timing.rpcConversionTicks += ReadPerformanceCounter() - conversionStart;
    NamedPipeRpcSendTiming sendTiming{};
    ++state.timing.terminalMessages;
    ++state.timing.physicalRpcMessages;
    const bool sent = server.SendEventMessage(RpcEventMessage(canceledEvent), &sendTiming);
    state.timing.rpcSerializeTicks += sendTiming.serializeTicks;
    state.timing.rpcWriteTicks += sendTiming.writeTicks;
    state.timing.rpcEnqueueWaitTicks += sendTiming.enqueueWaitTicks;
    state.timing.bytesWritten += sendTiming.bytesWritten;
    return sent;
}

bool RemoteScanHost::SendCompleted(NamedPipeRpcServer& server, ActiveRequestState& state)
{
    const std::uint64_t conversionStart = ReadPerformanceCounter();
    RpcScanCompletedEvent completed{};
    completed.requestId = state.requestId;
    state.timing.rpcConversionTicks += ReadPerformanceCounter() - conversionStart;
    NamedPipeRpcSendTiming sendTiming{};
    ++state.timing.terminalMessages;
    ++state.timing.physicalRpcMessages;
    const bool sent = server.SendEventMessage(RpcEventMessage(completed), &sendTiming);
    state.timing.rpcSerializeTicks += sendTiming.serializeTicks;
    state.timing.rpcWriteTicks += sendTiming.writeTicks;
    state.timing.rpcEnqueueWaitTicks += sendTiming.enqueueWaitTicks;
    state.timing.bytesWritten += sendTiming.bytesWritten;
    return sent;
}

bool RemoteScanHost::SendFailed(NamedPipeRpcServer& server, ActiveRequestState& state, const ScanSessionFailure& failure)
{
    const std::uint64_t conversionStart = ReadPerformanceCounter();
    RpcScanFailedEvent failed{};
    failed.requestId = state.requestId;
    failed.path = failure.path;
    failed.message = failure.message;
    failed.errorCode = failure.errorCode;
    state.timing.rpcConversionTicks += ReadPerformanceCounter() - conversionStart;
    NamedPipeRpcSendTiming sendTiming{};
    ++state.timing.terminalMessages;
    ++state.timing.physicalRpcMessages;
    const bool sent = server.SendEventMessage(RpcEventMessage(std::move(failed)), &sendTiming);
    state.timing.rpcSerializeTicks += sendTiming.serializeTicks;
    state.timing.rpcWriteTicks += sendTiming.writeTicks;
    state.timing.rpcEnqueueWaitTicks += sendTiming.enqueueWaitTicks;
    state.timing.bytesWritten += sendTiming.bytesWritten;
    return sent;
}

DirectoryWorkItemResult RemoteScanHost::ProcessDirectoryWorkItem(const ActiveRequestState& state, const std::wstring& path)
{
    ASSERT(state.requestId != 0);
    RpcHostLog(state.requestId, L"EngineInvocationStarted", std::format(L"path=\"{}\"", path));
    ASSERT(m_directoryProcessor != nullptr);
    return m_directoryProcessor->Process(path);
}

DirectoryWorkItemPolicyResult RemoteScanHost::ApplyDirectoryWorkItemPolicy(
    const ActiveRequestState& state,
    DirectoryWorkItemResult workItemResult)
{
    ASSERT(state.requestId != 0);
    (void)state;

    DirectoryWorkItemPolicyInput input{};
    input.workItemResult = std::move(workItemResult);

    return m_directoryPolicy.Apply(std::move(input));
}

RemoteScanHost::DirectoryWorkItemEffectsResult RemoteScanHost::ApplyDirectoryWorkItemEffects(
    NamedPipeRpcServer& server,
    ActiveRequestState& state,
    DirectoryWorkItemPolicyResult policyResult)
{
    const std::uint64_t requestId = state.requestId;
    ASSERT(requestId != 0);

    if (const auto* failure = std::get_if<DirectoryFailurePayload>(&policyResult.payload))
    {
        ScanSessionFailure sessionFailure{};
        sessionFailure.path = failure->path;
        sessionFailure.message = failure->message;
        sessionFailure.errorCode = failure->errorCode;
        state.session.ReportDirectoryFailure(sessionFailure);
        DirectoryWorkItemEffectsResult effectsResult{};
        effectsResult.discoveryResult = DiscoveryResult::RequestFailed;
        effectsResult.failure = std::move(sessionFailure);
        return effectsResult;
    }

    auto& progressPayload = std::get<DirectoryProgressPayload>(policyResult.payload);
    std::vector<TraversalChildCandidate> childCandidates;
    childCandidates.reserve(progressPayload.directories.size());
    for (const auto& directory : progressPayload.directories)
    {
        TraversalChildCandidate candidate{};
        candidate.fullPath = directory.fullPath;
        candidate.reparseTag = directory.reparseTag;
        candidate.isProtectedReparsePoint = directory.isProtectedReparsePoint;
        childCandidates.push_back(std::move(candidate));
    }

    const ScanSessionDirectoryReport directoryReport = state.session.ReportDirectoryResult(childCandidates);
    const std::size_t queuedChildren = directoryReport.scheduledChildPaths.size();

    if (queuedChildren != 0)
    {
        RpcHostLog(requestId, L"TraversalQueuedChildren",
            std::format(L"path=\"{}\" queuedChildren={} queueSize={}",
                progressPayload.directoryPath, queuedChildren, state.session.PendingWorkCount()),
            ScanHostLogger::Level::Verbose);
    }

    const std::uint64_t conversionStart = ReadPerformanceCounter();
    RpcDirectoryProgressEvent progress{};
    progress.requestId = requestId;
    progress.directoryPath = std::move(progressPayload.directoryPath);
    progress.files = std::move(progressPayload.files);
    progress.directories = std::move(progressPayload.directories);
    progress.finished = progressPayload.finished;
    state.timing.rpcConversionTicks += ReadPerformanceCounter() - conversionStart;
    RpcHostLog(requestId, L"DirectoryProgressEmitted",
        std::format(L"path=\"{}\" files={} directories={} finished=1",
            progress.directoryPath, progress.files.size(), progress.directories.size()),
        ScanHostLogger::Level::Verbose);
    if (SendOrBufferDirectoryProgress(server, state, std::move(progress)) == DiscoveryResult::TransportFailure)
    {
        DirectoryWorkItemEffectsResult effectsResult{};
        effectsResult.discoveryResult = DiscoveryResult::TransportFailure;
        effectsResult.queuedChildren = queuedChildren;
        return effectsResult;
    }

    DirectoryWorkItemEffectsResult effectsResult{};
    effectsResult.discoveryResult = DiscoveryResult::ProgressSent;
    effectsResult.queuedChildren = queuedChildren;
    return effectsResult;
}

RemoteScanHost::DiscoveryResult RemoteScanHost::SendOrBufferDirectoryProgress(
    NamedPipeRpcServer& server,
    ActiveRequestState& state,
    RpcDirectoryProgressEvent progress)
{
    ++state.timing.directoryProgressMessages;

    if (!state.batchProgressEnabled)
    {
        NamedPipeRpcSendTiming sendTiming{};
        ++state.timing.physicalRpcMessages;
        const bool sent = server.SendEventMessage(RpcEventMessage(std::move(progress)), &sendTiming);
        state.timing.rpcSerializeTicks += sendTiming.serializeTicks;
        state.timing.rpcWriteTicks += sendTiming.writeTicks;
        state.timing.rpcEnqueueWaitTicks += sendTiming.enqueueWaitTicks;
        state.timing.bytesWritten += sendTiming.bytesWritten;
        return sent ? DiscoveryResult::ProgressSent : DiscoveryResult::TransportFailure;
    }

    if (state.pendingProgressBatch.empty())
        state.pendingProgressBatchStartedTicks = ReadPerformanceCounter();

    state.pendingProgressBatch.push_back(std::move(progress));
    state.timing.maxBatchSize = std::max<std::uint64_t>(
        state.timing.maxBatchSize,
        static_cast<std::uint64_t>(state.pendingProgressBatch.size()));

    if (state.pendingProgressBatch.size() >= state.batchProgressSize)
        return FlushDirectoryProgressBatch(server, state) ? DiscoveryResult::ProgressSent : DiscoveryResult::TransportFailure;

    return DiscoveryResult::ProgressSent;
}

bool RemoteScanHost::FlushDirectoryProgressBatch(NamedPipeRpcServer& server, ActiveRequestState& state)
{
    if (state.pendingProgressBatch.empty())
        return true;

    RpcDirectoryProgressBatchEvent batch{};
    batch.requestId = state.requestId;
    batch.items = std::move(state.pendingProgressBatch);
    state.pendingProgressBatch.clear();
    state.pendingProgressBatch.reserve(state.batchProgressSize);
    state.pendingProgressBatchStartedTicks = 0;

    NamedPipeRpcSendTiming sendTiming{};
    ++state.timing.physicalRpcMessages;
    ++state.timing.directoryProgressBatchMessages;
    const bool sent = server.SendEventMessage(RpcEventMessage(std::move(batch)), &sendTiming);
    state.timing.rpcSerializeTicks += sendTiming.serializeTicks;
    state.timing.rpcWriteTicks += sendTiming.writeTicks;
    state.timing.rpcEnqueueWaitTicks += sendTiming.enqueueWaitTicks;
    state.timing.bytesWritten += sendTiming.bytesWritten;
    return sent;
}

bool RemoteScanHost::ShouldFlushDirectoryProgressBatchByDelay(const ActiveRequestState& state) const
{
    if (!state.batchProgressEnabled || state.pendingProgressBatch.empty())
        return false;
    if (state.batchProgressMaxDelayTicks == 0)
        return true;

    return ReadPerformanceCounter() - state.pendingProgressBatchStartedTicks >= state.batchProgressMaxDelayTicks;
}

void RemoteScanHost::EmitTimingSummary(NamedPipeRpcServer& server, const ActiveRequestState& state, const ScanSessionAction& terminalAction)
{
    if (!IsTimingEnabled())
        return;

    ASSERT(state.requestId != 0);
    ScanHostTimingCounters timing = state.timing;
    const NamedPipeRpcSendTiming asyncTiming = server.GetAsyncSendTiming();
    timing.rpcSerializeTicks += asyncTiming.serializeTicks;
    timing.rpcWriteTicks += asyncTiming.writeTicks;
    timing.bytesWritten += asyncTiming.bytesWritten;
    timing.maxBatchSize = (std::max)(timing.maxBatchSize, asyncTiming.maxQueueDepth);
    timing.hostTotalTicks = ReadPerformanceCounter() - timing.requestStartedTicks;
    const ScanSessionTimingStats rustStats = state.session.TimingStats();
    const double hostTotalMs = PerformanceTicksToMilliseconds(timing.hostTotalTicks);
    const double nextActionMs = PerformanceTicksToMilliseconds(timing.nextActionTicks);
    const double conversionMs = PerformanceTicksToMilliseconds(timing.rpcConversionTicks);
    const double serializeMs = PerformanceTicksToMilliseconds(timing.rpcSerializeTicks);
    const double writeMs = PerformanceTicksToMilliseconds(timing.rpcWriteTicks);
    const double enqueueWaitMs = PerformanceTicksToMilliseconds(timing.rpcEnqueueWaitTicks);
    const double averageBatchSize = timing.directoryProgressBatchMessages == 0
        ? 0.0
        : static_cast<double>(timing.directoryProgressMessages) / static_cast<double>(timing.directoryProgressBatchMessages);

    std::wstring terminal = L"unknown";
    switch (terminalAction.kind)
    {
    case ScanSessionActionKind::Complete:
        terminal = L"completed";
        break;
    case ScanSessionActionKind::Cancelled:
        terminal = L"cancelled";
        break;
    case ScanSessionActionKind::Failed:
        terminal = L"failed";
        break;
    default:
        break;
    }

    // Intent: single-line milestone timing for scan-host performance slices.
    // Contract: this is host-log instrumentation only; it does not change the
    // RPC protocol or move ownership across the C++ adapter/Rust engine boundary.
    RpcHostLog(state.requestId, L"TimingSummary",
        std::format(
            L"{{\"requestId\":{},\"terminal\":\"{}\",\"batchProgressEnabled\":{},\"hostTotalMs\":{:.3f},\"nextActionMs\":{:.3f},\"rpcConversionMs\":{:.3f},\"rpcSerializeMs\":{:.3f},\"rpcWriteMs\":{:.3f},\"rpcEnqueueWaitMs\":{:.3f},\"directoryProgressMessages\":{},\"physicalRpcMessages\":{},\"directoryProgressBatchMessages\":{},\"terminalMessages\":{},\"averageBatchSize\":{:.3f},\"maxBatchSize\":{},\"outboundQueueMessagesEnqueued\":{},\"outboundQueueMessagesWritten\":{},\"outboundQueueMaxDepth\":{},\"bytesWritten\":{},\"hostWaitingPollCount\":{},\"rustDiscoveryWorkerElapsedMs\":{:.3f},\"rustSchedulingElapsedMs\":{:.3f},\"rustNoEventPollCount\":{},\"rustWorkerCount\":{}}}",
            state.requestId,
            terminal,
            state.batchProgressEnabled ? L"true" : L"false",
            hostTotalMs,
            nextActionMs,
            conversionMs,
            serializeMs,
            writeMs,
            enqueueWaitMs,
            timing.directoryProgressMessages,
            timing.physicalRpcMessages,
            timing.directoryProgressBatchMessages,
            timing.terminalMessages,
            averageBatchSize,
            timing.maxBatchSize,
            asyncTiming.messagesEnqueued,
            asyncTiming.messagesWritten,
            asyncTiming.maxQueueDepth,
            timing.bytesWritten,
            timing.waitingPollCount,
            static_cast<double>(rustStats.discoveryWorkerElapsedNs) / 1'000'000.0,
            static_cast<double>(rustStats.schedulingElapsedNs) / 1'000'000.0,
            rustStats.noEventPollCount,
            rustStats.workerCount));
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
