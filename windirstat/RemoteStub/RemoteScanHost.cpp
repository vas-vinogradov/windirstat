#include "pch.h"
#include "RemoteStub/RemoteScanHost.h"

#include "LegacyDiscoveryRequest.h"
#include "RemoteStub/NamedPipeRpcServer.h"
#include "RemoteStub/StubDirectoryDiscoveryEngine.h"

namespace
{
std::wstring RpcTimestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

void RpcHostLog(std::uint64_t requestId, std::wstring_view event, std::wstring_view details = {})
{
    std::wstring line = std::format(L"[RPC][HOST] ts={} request={} event={}",
        RpcTimestamp(), requestId, event);
    if (!details.empty())
        line += std::format(L" {}", details);

    line += L"\n";
    OutputDebugStringW(line.c_str());
}

bool UseStubDiscoveryEngine()
{
    std::array<wchar_t, 16> buffer{};
    const DWORD length = GetEnvironmentVariable(L"WINDIRSTAT_REMOTE_DISCOVERY_ENGINE", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return false;

    std::wstring value(buffer.data(), length);
    _wcslwr_s(value.data(), value.size() + 1);
    return value == L"stub";
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
    NamedPipeRpcServer server(pipeName);
    if (!server.Listen())
        return 2;

    ActiveRequestState activeRequest{};
    std::optional<RpcStartScanRequest> queuedStart;

    while (true)
    {
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
                std::format(L"path=\"{}\" queueSize={}", path, activeRequest.pendingPaths.size()));

            switch (ExecuteDiscovery(server, activeRequest.requestId, path))
            {
            case DiscoveryResult::ProgressSent:
                RpcHostLog(activeRequest.requestId, L"DirectoryProcessingFinished",
                    std::format(L"path=\"{}\" result=ProgressSent queueSize={}", path, activeRequest.pendingPaths.size()));
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

        std::visit([&](const auto& message)
        {
            using TMessage = std::decay_t<decltype(message)>;

            if constexpr (std::is_same_v<TMessage, RpcStartScanRequest>)
            {
                if (activeRequest.requestId == 0)
                {
                    ASSERT(message.requestId != 0);
                    RpcHostLog(message.requestId, L"StartScanReceived",
                        std::format(L"path=\"{}\"", message.rootPath));
                    StartActiveRequest(activeRequest, message.requestId, message.rootPath);
                }
                else
                {
                    ASSERT(activeRequest.requestId != 0);
                    ASSERT(message.requestId != 0);
                    ASSERT(activeRequest.requestId != message.requestId);
                    // The host is intentionally a single-session, single-active-request loop.
                    // Replacement is explicit: cancel the old request first, then activate the
                    // queued replacement after the old request has emitted its terminal event.
                    RpcHostLog(activeRequest.requestId, L"ReplacementTriggered",
                        std::format(L"replacement={} path=\"{}\"", message.requestId, message.rootPath));
                    queuedStart = message;
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
                if (message.requestId == activeRequest.requestId && !activeRequest.inputClosed && !activeRequest.cancelPending)
                {
                    ASSERT(activeRequest.requestId != 0);
                    RpcHostLog(message.requestId, L"EnqueueReceived",
                        std::format(L"path=\"{}\" queueSize={}", message.rootPath, activeRequest.pendingPaths.size() + 1));
                    activeRequest.pendingPaths.push_back(message.rootPath);
                }
            }

            if constexpr (std::is_same_v<TMessage, RpcCloseRequestInput>)
            {
                if (message.requestId == activeRequest.requestId && !activeRequest.cancelPending)
                {
                    ASSERT(activeRequest.requestId != 0);
                    RpcHostLog(message.requestId, L"CloseRequestInputReceived",
                        std::format(L"queueSize={}", activeRequest.pendingPaths.size()));
                    activeRequest.inputClosed = true;
                }
            }

            if constexpr (std::is_same_v<TMessage, RpcCancelScanRequest>)
            {
                if (message.requestId == activeRequest.requestId)
                {
                    ASSERT(activeRequest.requestId != 0);
                    RpcHostLog(message.requestId, L"CancelReceived",
                        std::format(L"reason={} queueSize={}", static_cast<int>(message.reason), activeRequest.pendingPaths.size()));
                    activeRequest.cancelPending = true;
                    activeRequest.cancelReason = message.reason;
                    activeRequest.inputClosed = true;
                    activeRequest.pendingPaths.clear();
                    queuedStart.reset();
                    ASSERT(activeRequest.pendingPaths.empty());
                }
            }
        }, messageOpt.value());
    }

    return 0;
}

void RemoteScanHost::ResetActiveRequest(ActiveRequestState& state)
{
    const std::uint64_t requestId = state.requestId;
    ASSERT(requestId != 0);
    state.requestId = 0;
    state.pendingPaths.clear();
    state.inputClosed = false;
    state.cancelPending = false;
    state.cancelReason = ScanTerminalReason::EngineInterrupted;
    ASSERT(state.requestId == 0);
    ASSERT(state.pendingPaths.empty());
    ASSERT(!state.inputClosed);
    ASSERT(!state.cancelPending);
    RpcHostLog(requestId, L"RequestReset", L"queueSize=0 inputClosed=0 cancelPending=0");
}

void RemoteScanHost::StartActiveRequest(ActiveRequestState& state, const std::uint64_t requestId, const std::wstring& rootPath)
{
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
    state.cancelReason = ScanTerminalReason::EngineInterrupted;
    RpcHostLog(requestId, L"RequestActivated",
        std::format(L"path=\"{}\" queueSize=1 inputClosed=0 cancelPending=0", rootPath));
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
    StartActiveRequest(state, nextStart.requestId, nextStart.rootPath);
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

RemoteScanHost::DiscoveryResult RemoteScanHost::ExecuteDiscovery(NamedPipeRpcServer& server, const std::uint64_t requestId, const std::wstring& path)
{
    ASSERT(requestId != 0);
    try
    {
        LegacyDiscoveryRequest request{};
        request.path = path;
        request.ntfsContext = &m_contextNtfs;
        request.basicContext = &m_contextBasic;

        ASSERT(m_discoveryEngine != nullptr);
        DiscoveryBatch discoveryBatch = m_discoveryEngine->Discover(request);

        RpcDirectoryProgressEvent progress{};
        progress.requestId = requestId;
        progress.directoryPath = path;
        progress.files = std::move(discoveryBatch.files);
        progress.directories = std::move(discoveryBatch.directories);
        progress.finished = true;
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

std::unique_ptr<IDirectoryDiscoveryEngine> RemoteScanHost::CreateDiscoveryEngine()
{
    if (UseStubDiscoveryEngine())
        return std::make_unique<StubDirectoryDiscoveryEngine>();

    return std::make_unique<LegacyDiscoveryEngine>();
}

std::wstring RemoteScanHost::GetDiscoveryEngineMode()
{
    return UseStubDiscoveryEngine() ? L"stub" : L"legacy";
}
