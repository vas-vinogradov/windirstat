#pragma once

#include "Engine/Rpc/RpcMessages.h"

#include <optional>
#include <string>

struct NamedPipeRpcSendTiming
{
    std::uint64_t serializeTicks = 0;
    std::uint64_t writeTicks = 0;
    std::uint64_t enqueueWaitTicks = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t messagesEnqueued = 0;
    std::uint64_t messagesWritten = 0;
    std::uint64_t maxQueueDepth = 0;
};

class NamedPipeRpcServer
{
public:
    explicit NamedPipeRpcServer(std::wstring pipeName);
    ~NamedPipeRpcServer();

    bool Listen();
    bool HasPendingRequestMessage() const;
    std::optional<RpcRequestMessage> ReadRequestMessage();
    std::optional<RpcRequestMessage> ReadRequestMessage(DWORD timeoutMs);
    bool SendEventMessage(RpcEventMessage message, NamedPipeRpcSendTiming* timing = nullptr);
    bool FlushEventMessages();
    NamedPipeRpcSendTiming GetAsyncSendTiming() const;

private:
    struct QueuedEventMessage
    {
        RpcEventMessage message;
    };

    bool WriteJsonMessage(const std::string& json);
    bool WriteEventMessageNow(const RpcEventMessage& message, NamedPipeRpcSendTiming* timing);
    std::optional<std::string> ReadJsonMessage();
    void StartWriterThreadIfConfigured();
    void WriterLoop();
    bool EnqueueEventMessage(RpcEventMessage message, NamedPipeRpcSendTiming* timing);
    static std::size_t GetOutboundQueueCapacity();
    void Close();

    std::wstring m_pipeName;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::size_t m_outboundQueueCapacity = 0;
    mutable std::mutex m_writerMutex;
    std::condition_variable m_writerCanPush;
    std::condition_variable m_writerCanPop;
    std::condition_variable m_writerDrained;
    std::deque<QueuedEventMessage> m_outboundQueue;
    std::optional<std::jthread> m_writerThread;
    NamedPipeRpcSendTiming m_asyncTiming;
    bool m_writerStopping = false;
    bool m_writerFailed = false;
    bool m_writerActive = false;
};
