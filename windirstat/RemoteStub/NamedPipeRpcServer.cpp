#include "pch.h"
#include "RemoteStub/NamedPipeRpcServer.h"
#include "RemoteStub/ScanHostLogger.h"

namespace
{
std::uint64_t ReadPerformanceCounter()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

bool WriteAll(HANDLE handle, const void* buffer, const DWORD bytesToWrite)
{
    DWORD totalWritten = 0;
    while (totalWritten < bytesToWrite)
    {
        DWORD written = 0;
        if (!WriteFile(handle, static_cast<const BYTE*>(buffer) + totalWritten, bytesToWrite - totalWritten, &written, nullptr))
            return false;

        totalWritten += written;
    }

    return true;
}

bool ReadAll(HANDLE handle, void* buffer, const DWORD bytesToRead)
{
    DWORD totalRead = 0;
    while (totalRead < bytesToRead)
    {
        DWORD read = 0;
        if (!ReadFile(handle, static_cast<BYTE*>(buffer) + totalRead, bytesToRead - totalRead, &read, nullptr))
            return false;

        if (read == 0)
            return false;

        totalRead += read;
    }

    return true;
}

std::size_t ReadQueueCapacityFromEnvironment()
{
    constexpr std::wstring_view variableName = L"WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY";
    std::array<wchar_t, 32> value{};
    const DWORD length = GetEnvironmentVariableW(variableName.data(), value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
        return 0;

    try
    {
        return std::clamp<std::size_t>(std::stoull(std::wstring(value.data(), length)), 0, 4096);
    }
    catch (...)
    {
        return 0;
    }
}
}

NamedPipeRpcServer::NamedPipeRpcServer(std::wstring pipeName)
    : m_pipeName(std::move(pipeName))
    , m_outboundQueueCapacity(GetOutboundQueueCapacity())
{
}

NamedPipeRpcServer::~NamedPipeRpcServer()
{
    FlushEventMessages();
    Close();
}

bool NamedPipeRpcServer::Listen()
{
    m_pipe = CreateNamedPipe(m_pipeName.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    ScanHostLogger::Log(std::format(L"[HOST] PipeListening pipe=\"{}\"", m_pipeName));
    ScanHostLogger::Log(std::format(L"[HOST] WaitingForClientConnection pipe=\"{}\"", m_pipeName));
    const BOOL connected = ConnectNamedPipe(m_pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
    if (connected == TRUE)
    {
        ScanHostLogger::Log(std::format(L"[HOST] ClientConnected pipe=\"{}\"", m_pipeName));
        StartWriterThreadIfConfigured();
    }
    return connected == TRUE;
}

bool NamedPipeRpcServer::HasPendingRequestMessage() const
{
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    DWORD bytesAvailable = 0;
    if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr))
        return false;

    return bytesAvailable >= sizeof(DWORD);
}

std::optional<RpcRequestMessage> NamedPipeRpcServer::ReadRequestMessage()
{
    const auto jsonOpt = ReadJsonMessage();
    if (!jsonOpt.has_value())
        return std::nullopt;

    return TryDeserializeRpcRequestMessage(jsonOpt.value());
}

std::optional<RpcRequestMessage> NamedPipeRpcServer::ReadRequestMessage(const DWORD timeoutMs)
{
    if (m_pipe == INVALID_HANDLE_VALUE)
        return std::nullopt;

    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs)
    {
        DWORD bytesAvailable = 0;
        if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr))
            return std::nullopt;

        if (bytesAvailable >= sizeof(DWORD))
            return ReadRequestMessage();

        Sleep(5);
    }

    return std::optional<RpcRequestMessage>{};
}

bool NamedPipeRpcServer::SendEventMessage(RpcEventMessage message, NamedPipeRpcSendTiming* const timing)
{
    if (m_outboundQueueCapacity != 0)
        return EnqueueEventMessage(std::move(message), timing);

    return WriteEventMessageNow(message, timing);
}

bool NamedPipeRpcServer::FlushEventMessages()
{
    if (m_outboundQueueCapacity == 0)
        return !m_writerFailed;

    std::unique_lock lock(m_writerMutex);
    m_writerDrained.wait(lock, [&]
    {
        return (m_outboundQueue.empty() && !m_writerActive) || m_writerFailed;
    });
    return !m_writerFailed;
}

NamedPipeRpcSendTiming NamedPipeRpcServer::GetAsyncSendTiming() const
{
    std::scoped_lock lock(m_writerMutex);
    return m_asyncTiming;
}

bool NamedPipeRpcServer::WriteEventMessageNow(const RpcEventMessage& message, NamedPipeRpcSendTiming* const timing)
{
    const std::uint64_t serializeStart = ReadPerformanceCounter();
    const std::string json = SerializeRpcEventMessage(message);
    const std::uint64_t serializeEnd = ReadPerformanceCounter();

    const std::uint64_t writeStart = ReadPerformanceCounter();
    const bool sent = WriteJsonMessage(json);
    const std::uint64_t writeEnd = ReadPerformanceCounter();

    if (timing != nullptr)
    {
        timing->serializeTicks += serializeEnd - serializeStart;
        timing->writeTicks += writeEnd - writeStart;
        timing->bytesWritten += sizeof(DWORD) + json.size();
        timing->messagesWritten += sent ? 1 : 0;
    }

    return sent;
}

void NamedPipeRpcServer::StartWriterThreadIfConfigured()
{
    if (m_outboundQueueCapacity == 0 || m_writerThread.has_value())
        return;

    // Intent: optional transport-only decoupling. The scan session still owns
    // scan progression; this thread only serializes and writes already-produced
    // RPC events in FIFO order.
    ScanHostLogger::Log(std::format(L"[HOST] OutboundQueueEnabled capacity={}", m_outboundQueueCapacity));
    m_writerThread.emplace([this]()
    {
        WriterLoop();
    });
}

void NamedPipeRpcServer::WriterLoop()
{
    while (true)
    {
        QueuedEventMessage queued{};
        {
            std::unique_lock lock(m_writerMutex);
            m_writerCanPop.wait(lock, [&]
            {
                return m_writerStopping || !m_outboundQueue.empty();
            });

            if (m_outboundQueue.empty())
            {
                if (m_writerStopping)
                    break;
                continue;
            }

            queued = std::move(m_outboundQueue.front());
            m_outboundQueue.pop_front();
            m_writerActive = true;
            m_writerCanPush.notify_one();
        }

        NamedPipeRpcSendTiming timing{};
        const bool written = WriteEventMessageNow(queued.message, &timing);

        {
            std::scoped_lock lock(m_writerMutex);
            m_asyncTiming.serializeTicks += timing.serializeTicks;
            m_asyncTiming.writeTicks += timing.writeTicks;
            m_asyncTiming.bytesWritten += timing.bytesWritten;
            m_asyncTiming.messagesWritten += timing.messagesWritten;
            m_writerActive = false;
            if (!written)
                m_writerFailed = true;
        }

        m_writerDrained.notify_all();
        m_writerCanPush.notify_all();
        if (!written)
            break;
    }

    m_writerDrained.notify_all();
    m_writerCanPush.notify_all();
}

bool NamedPipeRpcServer::EnqueueEventMessage(RpcEventMessage message, NamedPipeRpcSendTiming* const timing)
{
    const std::uint64_t enqueueStart = ReadPerformanceCounter();
    std::unique_lock lock(m_writerMutex);
    m_writerCanPush.wait(lock, [&]
    {
        return m_writerStopping || m_writerFailed || m_outboundQueue.size() < m_outboundQueueCapacity;
    });
    const std::uint64_t enqueueEnd = ReadPerformanceCounter();

    if (m_writerStopping || m_writerFailed)
        return false;

    m_outboundQueue.push_back(QueuedEventMessage{ std::move(message) });
    const std::uint64_t depth = static_cast<std::uint64_t>(m_outboundQueue.size());
    m_asyncTiming.enqueueWaitTicks += enqueueEnd - enqueueStart;
    ++m_asyncTiming.messagesEnqueued;
    m_asyncTiming.maxQueueDepth = (std::max)(m_asyncTiming.maxQueueDepth, depth);

    if (timing != nullptr)
    {
        timing->enqueueWaitTicks += enqueueEnd - enqueueStart;
        ++timing->messagesEnqueued;
        timing->maxQueueDepth = depth;
    }

    lock.unlock();
    m_writerCanPop.notify_one();
    return true;
}

std::size_t NamedPipeRpcServer::GetOutboundQueueCapacity()
{
    return ReadQueueCapacityFromEnvironment();
}

bool NamedPipeRpcServer::WriteJsonMessage(const std::string& json)
{
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    const DWORD length = static_cast<DWORD>(json.size());
    return WriteAll(m_pipe, &length, sizeof(length)) && (length == 0 || WriteAll(m_pipe, json.data(), length));
}

std::optional<std::string> NamedPipeRpcServer::ReadJsonMessage()
{
    if (m_pipe == INVALID_HANDLE_VALUE)
        return std::nullopt;

    DWORD length = 0;
    if (!ReadAll(m_pipe, &length, sizeof(length)))
        return std::nullopt;

    std::string json(length, '\0');
    if (length != 0 && !ReadAll(m_pipe, json.data(), length))
        return std::nullopt;

    return json;
}

void NamedPipeRpcServer::Close()
{
    {
        std::scoped_lock lock(m_writerMutex);
        m_writerStopping = true;
    }
    m_writerCanPop.notify_all();
    m_writerCanPush.notify_all();
    if (m_writerThread.has_value() && m_writerThread->joinable())
        m_writerThread->join();
    m_writerThread.reset();

    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}
