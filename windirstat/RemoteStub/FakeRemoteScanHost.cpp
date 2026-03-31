#include "pch.h"
#include "RemoteStub/FakeRemoteScanHost.h"

#include "LegacyDiscoveryRequest.h"
#include "RemoteStub/NamedPipeRpcServer.h"

int FakeRemoteScanHost::Run(const std::wstring& pipeName)
{
    NamedPipeRpcServer server(pipeName);
    if (!server.Listen())
        return 2;

    std::deque<std::wstring> pendingPaths;
    std::uint64_t activeRequestId = 0;
    bool inputClosed = false;
    bool canceled = false;
    ScanTerminalReason canceledReason = ScanTerminalReason::EngineInterrupted;

    while (true)
    {
        if (activeRequestId != 0 && canceled)
        {
            RpcScanCanceledEvent canceledEvent{};
            canceledEvent.requestId = activeRequestId;
            canceledEvent.reason = canceledReason;
            (void)server.SendEventMessage(RpcEventMessage(canceledEvent));
            activeRequestId = 0;
            inputClosed = false;
            canceled = false;
            pendingPaths.clear();
            continue;
        }

        if (activeRequestId != 0 && inputClosed && pendingPaths.empty())
        {
            RpcScanCompletedEvent completed{};
            completed.requestId = activeRequestId;
            (void)server.SendEventMessage(RpcEventMessage(completed));
            activeRequestId = 0;
            inputClosed = false;
            canceled = false;
            continue;
        }

        if (!pendingPaths.empty())
        {
            const std::wstring path = std::move(pendingPaths.front());
            pendingPaths.pop_front();
            if (!ExecuteDiscovery(server, activeRequestId, path))
                return 3;
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
                activeRequestId = message.requestId;
                pendingPaths.clear();
                inputClosed = false;
                canceled = false;
                pendingPaths.push_back(message.rootPath);
            }

            if constexpr (std::is_same_v<TMessage, RpcEnqueueRequest>)
            {
                if (message.requestId == activeRequestId && !inputClosed && !canceled)
                    pendingPaths.push_back(message.rootPath);
            }

            if constexpr (std::is_same_v<TMessage, RpcCloseRequestInput>)
            {
                if (message.requestId == activeRequestId && !canceled)
                    inputClosed = true;
            }

            if constexpr (std::is_same_v<TMessage, RpcCancelScanRequest>)
            {
                if (message.requestId == activeRequestId)
                {
                    canceled = true;
                    canceledReason = message.reason;
                    inputClosed = true;
                    pendingPaths.clear();
                }
            }
        }, messageOpt.value());
    }

    return 0;
}

bool FakeRemoteScanHost::ExecuteDiscovery(NamedPipeRpcServer& server, const std::uint64_t requestId, const std::wstring& path)
{
    try
    {
        LegacyDiscoveryRequest request{};
        request.path = path;
        request.ntfsContext = &m_contextNtfs;
        request.basicContext = &m_contextBasic;

        DiscoveryBatch discoveryBatch = m_discoveryEngine.Discover(request);

        RpcDirectoryProgressEvent progress{};
        progress.requestId = requestId;
        progress.directoryPath = path;
        progress.files = std::move(discoveryBatch.files);
        progress.directories = std::move(discoveryBatch.directories);
        progress.finished = true;
        return server.SendEventMessage(RpcEventMessage(std::move(progress)));
    }
    catch (const std::exception& ex)
    {
        RpcScanFailedEvent failed{};
        failed.requestId = requestId;
        failed.path = path;
        failed.message = std::wstring(CA2W(ex.what()));
        failed.errorCode = 0;
        return server.SendEventMessage(RpcEventMessage(std::move(failed)));
    }
}
