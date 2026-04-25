#pragma once

#include "Engine/Rpc/IRpcTransportClient.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class NamedPipeRpcClient final : public IRpcTransportClient
{
public:
    NamedPipeRpcClient();
    ~NamedPipeRpcClient() override;

    void SetEventHandler(IRpcTransportEventHandler* handler) override;
    bool SendStartScan(const RpcStartScanRequest& request) override;
    bool SendEnqueue(const RpcEnqueueRequest& request) override;
    bool SendCloseRequestInput(const RpcCloseRequestInput& request) override;
    bool SendCancelScan(const RpcCancelScanRequest& request) override;

private:
    struct InboundQueueStats
    {
        std::uint64_t messagesEnqueued = 0;
        std::uint64_t messagesProcessed = 0;
        std::uint64_t maxDepth = 0;
        std::uint64_t enqueueWaitTicks = 0;
        std::uint64_t processingTicks = 0;
    };

    struct InboundFrame
    {
        std::string json;
    };

    bool EnsureConnected();
    bool EnsureHostRunning();
    bool WaitUntilHostReady();
    bool ConnectToHost();
    bool SendJsonMessage(const std::string& json);
    std::optional<std::string> ReadJsonMessage(bool notifyFailure = true);
    void ReaderLoop();
    void ProcessorLoop();
    bool EnqueueInboundFrame(std::string json);
    std::optional<InboundFrame> TakeInboundFrame();
    void StartInboundProcessorIfConfigured();
    void SignalInboundProcessorStop();
    void WaitForInboundProcessorDrain();
    void StopInboundProcessor();
    void TraceInboundQueueSummary() const;
    void AppendClientMetricsLog(std::wstring_view line) const;
    void NotifyTransportFailure(unsigned long errorCode, const std::wstring& message);
    void ClosePipe();
    void StopRemoteHost();
    void DispatchEvent(const RpcEventMessage& event);
    static std::wstring CreatePipeName();
    static std::wstring CreateHostLogFilePath();
    static bool IsVerboseLoggingEnabled();
    static std::size_t GetInboundQueueCapacity();
    static std::wstring GetClientMetricsLogFilePath();
    static std::wstring GetRemoteHostExecutablePath();

    std::wstring m_pipeName;
    std::wstring m_hostLogFilePath;
    std::wstring m_clientMetricsLogFilePath;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION m_processInfo{};
    mutable std::mutex m_ioMutex;
    mutable std::mutex m_connectMutex;
    mutable std::mutex m_handlerMutex;
    IRpcTransportEventHandler* m_handler = nullptr;
    std::optional<std::jthread> m_readerThread;
    std::optional<std::jthread> m_processorThread;
    std::size_t m_inboundQueueCapacity = 0;
    mutable std::mutex m_inboundQueueMutex;
    std::condition_variable m_inboundCanPush;
    std::condition_variable m_inboundCanPop;
    std::condition_variable m_inboundDrained;
    std::deque<InboundFrame> m_inboundQueue;
    InboundQueueStats m_inboundStats;
    bool m_inboundStopping = false;
    bool m_inboundProcessorStopped = true;
    unsigned long m_lastReadErrorCode = ERROR_SUCCESS;
    std::wstring m_lastReadErrorMessage;
    std::atomic_bool m_shutdownRequested = false;
    std::atomic_bool m_readerRunning = false;
    std::atomic_bool m_transportFailureNotified = false;
    bool m_verboseLogging = false;
};
