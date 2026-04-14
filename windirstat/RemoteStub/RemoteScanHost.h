#pragma once

#include "DirectoryDiscoveryEngine.h"
#include "Engine/Rpc/RpcMessages.h"

#include <deque>
#include <memory>
#include <optional>
#include <string>

class RemoteScanHost
{
public:
    RemoteScanHost();
    int Run(const std::wstring& pipeName);

private:
    struct ActiveRequestState
    {
        std::uint64_t requestId = 0;
        std::deque<std::wstring> pendingPaths{};
        bool inputClosed = false;
        bool cancelPending = false;
        bool followMountPoints = false;
        bool followSymbolicLinks = false;
        bool followJunctions = false;
        ScanTerminalReason cancelReason = ScanTerminalReason::EngineInterrupted;
    };

    enum class DiscoveryResult
    {
        ProgressSent,
        RequestFailed,
        TransportFailure
    };

    void ResetActiveRequest(ActiveRequestState& state);
    void StartActiveRequest(ActiveRequestState& state, const RpcStartScanRequest& request);
    void PromoteQueuedStart(ActiveRequestState& state, std::optional<RpcStartScanRequest>& queuedStart);
    void HandleRequestMessage(ActiveRequestState& activeRequest, std::optional<RpcStartScanRequest>& queuedStart, const RpcRequestMessage& message);
    bool SendCanceled(class NamedPipeRpcServer& server, const ActiveRequestState& state);
    bool SendCompleted(class NamedPipeRpcServer& server, const ActiveRequestState& state);
    DiscoveryResult ExecuteDiscovery(class NamedPipeRpcServer& server, ActiveRequestState& state, const std::wstring& path);
    static std::unique_ptr<IDirectoryDiscoveryEngine> CreateDiscoveryEngine();
    static std::wstring GetDiscoveryEngineMode();
    static bool ShouldQueueDiscoveredDirectory(const ActiveRequestState& state, const DiscoveredDirectory& directory);

    std::unique_ptr<IDirectoryDiscoveryEngine> m_discoveryEngine;
};
