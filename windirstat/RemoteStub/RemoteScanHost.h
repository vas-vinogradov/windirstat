#pragma once

#include "Engine/Rpc/RpcMessages.h"
#include "RemoteStub/DirectoryWorkItemHostPolicy.h"
#include "RemoteStub/DirectoryWorkItemProcessor.h"
#include "RemoteStub/ScanSessionState.h"

#include <memory>
#include <optional>
#include <string>

class RemoteScanHost
{
public:
    RemoteScanHost();
    int Run(const std::wstring& pipeName);

private:
    struct ScanHostTimingCounters
    {
        std::uint64_t requestStartedTicks = 0;
        std::uint64_t hostTotalTicks = 0;
        std::uint64_t nextActionTicks = 0;
        std::uint64_t rpcConversionTicks = 0;
        std::uint64_t rpcSerializeTicks = 0;
        std::uint64_t rpcWriteTicks = 0;
        std::uint64_t rpcEnqueueWaitTicks = 0;
        std::uint64_t bytesWritten = 0;
        std::uint64_t directoryProgressMessages = 0;
        std::uint64_t physicalRpcMessages = 0;
        std::uint64_t directoryProgressBatchMessages = 0;
        std::uint64_t terminalMessages = 0;
        std::uint64_t waitingPollCount = 0;
        std::uint64_t maxBatchSize = 0;
    };

    struct ActiveRequestState
    {
        std::uint64_t requestId = 0;
        ScanSessionState session;
        ScanHostTimingCounters timing;
        bool batchProgressEnabled = false;
        std::size_t batchProgressSize = 64;
        std::uint64_t batchProgressMaxDelayTicks = 0;
        std::uint64_t pendingProgressBatchStartedTicks = 0;
        std::vector<RpcDirectoryProgressEvent> pendingProgressBatch;

        explicit ActiveRequestState(bool enableRustSession)
            : session(enableRustSession)
        {
        }
    };

    enum class DiscoveryResult
    {
        ProgressSent,
        RequestFailed,
        TransportFailure
    };

    enum class TerminalEmissionResult
    {
        None,
        Sent,
        TransportFailure
    };

    struct DirectoryWorkItemEffectsResult
    {
        DiscoveryResult discoveryResult = DiscoveryResult::ProgressSent;
        std::size_t queuedChildren = 0;
        std::optional<ScanSessionFailure> failure;
    };

    void ResetActiveRequest(ActiveRequestState& state);
    void StartActiveRequest(ActiveRequestState& state, const RpcStartScanRequest& request);
    void PromoteQueuedStart(ActiveRequestState& state, std::optional<RpcStartScanRequest>& queuedStart);
    void HandleRequestMessage(ActiveRequestState& activeRequest, std::optional<RpcStartScanRequest>& queuedStart, const RpcRequestMessage& message);
    TerminalEmissionResult EmitTerminalAction(class NamedPipeRpcServer& server, ActiveRequestState& state, const ScanSessionAction& action);
    DiscoveryResult EmitDirectoryResultAction(class NamedPipeRpcServer& server, ActiveRequestState& state, const ScanSessionDirectoryResult& result);
    bool SendCanceled(class NamedPipeRpcServer& server, ActiveRequestState& state, ScanTerminalReason reason);
    bool SendCompleted(class NamedPipeRpcServer& server, ActiveRequestState& state);
    bool SendFailed(class NamedPipeRpcServer& server, ActiveRequestState& state, const ScanSessionFailure& failure);
    // Temporary: fallback-only discovery path kept for non-Rust or env-forced
    // compatibility execution. Rust-backed sessions must not call these.
    DirectoryWorkItemResult ProcessDirectoryWorkItem(const ActiveRequestState& state, const std::wstring& path);
    DirectoryWorkItemPolicyResult ApplyDirectoryWorkItemPolicy(const ActiveRequestState& state, DirectoryWorkItemResult workItemResult);
    DirectoryWorkItemEffectsResult ApplyDirectoryWorkItemEffects(class NamedPipeRpcServer& server, ActiveRequestState& state, DirectoryWorkItemPolicyResult policyResult);
    DiscoveryResult SendOrBufferDirectoryProgress(class NamedPipeRpcServer& server, ActiveRequestState& state, RpcDirectoryProgressEvent progress);
    bool FlushDirectoryProgressBatch(class NamedPipeRpcServer& server, ActiveRequestState& state);
    bool ShouldFlushDirectoryProgressBatchByDelay(const ActiveRequestState& state) const;
    void EmitTimingSummary(class NamedPipeRpcServer& server, const ActiveRequestState& state, const ScanSessionAction& terminalAction);
    static std::unique_ptr<IDirectoryDiscoveryEngine> CreateDiscoveryEngine();
    static std::wstring GetDiscoveryEngineMode();

    bool m_rustSessionOwnsDiscovery = false;
    DirectoryWorkItemHostPolicy m_directoryPolicy;
    std::unique_ptr<IDirectoryWorkItemProcessor> m_directoryProcessor;
};
