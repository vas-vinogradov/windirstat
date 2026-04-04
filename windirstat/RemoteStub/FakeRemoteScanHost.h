#pragma once

#include "Engine/Rpc/RpcMessages.h"
#include "FinderBasic.h"
#include "FinderNtfs.h"
#include "LegacyDiscoveryEngine.h"

#include <deque>
#include <optional>
#include <string>

class FakeRemoteScanHost
{
public:
    int Run(const std::wstring& pipeName);

private:
    struct ActiveRequestState
    {
        std::uint64_t requestId = 0;
        std::deque<std::wstring> pendingPaths{};
        bool inputClosed = false;
        bool cancelPending = false;
        ScanTerminalReason cancelReason = ScanTerminalReason::EngineInterrupted;
    };

    enum class DiscoveryResult
    {
        ProgressSent,
        RequestFailed,
        TransportFailure
    };

    void ResetActiveRequest(ActiveRequestState& state);
    void StartActiveRequest(ActiveRequestState& state, std::uint64_t requestId, const std::wstring& rootPath);
    void PromoteQueuedStart(ActiveRequestState& state, std::optional<RpcStartScanRequest>& queuedStart);
    bool SendCanceled(class NamedPipeRpcServer& server, const ActiveRequestState& state);
    bool SendCompleted(class NamedPipeRpcServer& server, const ActiveRequestState& state);
    DiscoveryResult ExecuteDiscovery(class NamedPipeRpcServer& server, std::uint64_t requestId, const std::wstring& path);

    FinderNtfsContext m_contextNtfs{};
    FinderBasicContext m_contextBasic{};
    LegacyDiscoveryEngine m_discoveryEngine{};
};
