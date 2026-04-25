#include "pch.h"
#include "Engine/Rpc/NamedPipeRpcClient.h"

#include "HelpersInterface.h"

namespace
{
constexpr DWORD kHostReadinessTimeoutMs = 5000;

bool IsRpcVerboseLoggingEnabledFromCommandLine()
{
    for (int index = 1; index < __argc; ++index)
    {
        const std::wstring arg = __wargv[index];
        if (arg == L"--rpc-log-level" && index + 1 < __argc)
        {
            std::wstring value = __wargv[++index];
            _wcslwr_s(value.data(), value.size() + 1);
            return value == L"verbose";
        }
    }

    return false;
}

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

std::size_t ReadInboundQueueCapacityFromEnvironment()
{
    constexpr std::wstring_view variableName = L"WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY";
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

bool IsTerminalEvent(const RpcEventMessage& event)
{
    return std::holds_alternative<RpcScanCompletedEvent>(event) ||
        std::holds_alternative<RpcScanCanceledEvent>(event) ||
        std::holds_alternative<RpcScanFailedEvent>(event);
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

void TraceOutgoingRequest(const RpcStartScanRequest& request)
{
    VTRACE(L"[RPC] Sending StartScan. request={} path=\"{}\"", request.requestId, request.rootPath);
}

void TraceOutgoingRequest(const RpcEnqueueRequest& request, const bool verbose)
{
    if (!verbose)
        return;

    VTRACE(L"[RPC] Sending Enqueue. request={} path=\"{}\"", request.requestId, request.rootPath);
}

void TraceOutgoingRequest(const RpcCloseRequestInput& request)
{
    VTRACE(L"[RPC] Sending CloseRequestInput. request={}", request.requestId);
}

void TraceOutgoingRequest(const RpcCancelScanRequest& request)
{
    VTRACE(L"[RPC] Sending CancelScan. request={} reason={}", request.requestId, static_cast<int>(request.reason));
}
}

NamedPipeRpcClient::NamedPipeRpcClient()
    : m_pipeName(CreatePipeName())
    , m_clientMetricsLogFilePath(GetClientMetricsLogFilePath())
    , m_inboundQueueCapacity(GetInboundQueueCapacity())
    , m_verboseLogging(IsVerboseLoggingEnabled())
{
    VTRACE(L"[RPC] NamedPipeRpcClient created. pipe={}", m_pipeName);
    AppendClientMetricsLog(std::format(L"[RPC] NamedPipeRpcClient created pipe={} inboundQueueCapacity={}",
        m_pipeName,
        m_inboundQueueCapacity));
}

NamedPipeRpcClient::~NamedPipeRpcClient()
{
    m_shutdownRequested.store(true, std::memory_order_release);
    m_readerRunning.store(false, std::memory_order_release);
    ClosePipe();

    if (m_readerThread.has_value() && m_readerThread->joinable())
        m_readerThread->join();
    m_readerThread.reset();
    StopInboundProcessor();

    StopRemoteHost();
}

void NamedPipeRpcClient::SetEventHandler(IRpcTransportEventHandler* handler)
{
    std::scoped_lock lock(m_handlerMutex);
    m_handler = handler;
}

bool NamedPipeRpcClient::SendStartScan(const RpcStartScanRequest& request)
{
    TraceOutgoingRequest(request);
    return EnsureConnected() && SendJsonMessage(SerializeRpcRequestMessage(RpcRequestMessage(request)));
}

bool NamedPipeRpcClient::SendEnqueue(const RpcEnqueueRequest& request)
{
    TraceOutgoingRequest(request, m_verboseLogging);
    return EnsureConnected() && SendJsonMessage(SerializeRpcRequestMessage(RpcRequestMessage(request)));
}

bool NamedPipeRpcClient::SendCloseRequestInput(const RpcCloseRequestInput& request)
{
    TraceOutgoingRequest(request);
    return EnsureConnected() && SendJsonMessage(SerializeRpcRequestMessage(RpcRequestMessage(request)));
}

bool NamedPipeRpcClient::SendCancelScan(const RpcCancelScanRequest& request)
{
    TraceOutgoingRequest(request);
    return EnsureConnected() && SendJsonMessage(SerializeRpcRequestMessage(RpcRequestMessage(request)));
}

bool NamedPipeRpcClient::EnsureConnected()
{
    std::scoped_lock connectLock(m_connectMutex);
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        if (m_verboseLogging)
            VTRACE(L"[RPC] EnsureConnected reused existing pipe. pipe={}", m_pipeName);
        return true;
    }

    m_transportFailureNotified.store(false, std::memory_order_release);
    VTRACE(L"[RPC] EnsureConnected starting host connection. pipe={}", m_pipeName);
    AppendClientMetricsLog(std::format(L"[RPC] EnsureConnected starting pipe={}", m_pipeName));

    if (!EnsureHostRunning())
    {
        VTRACE(L"[RPC] EnsureHostRunning failed. error={} message={}", GetLastError(), TranslateError());
        AppendClientMetricsLog(std::format(L"[RPC] EnsureHostRunning failed error={} message={}",
            GetLastError(),
            TranslateError()));
        NotifyTransportFailure(GetLastError(), L"Failed to start remote RPC host.");
        return false;
    }

    if (!WaitUntilHostReady())
    {
        VTRACE(L"[RPC] WaitUntilHostReady failed. error={} message={}", GetLastError(), TranslateError());
        AppendClientMetricsLog(std::format(L"[RPC] WaitUntilHostReady failed error={} message={}",
            GetLastError(),
            TranslateError()));
        NotifyTransportFailure(GetLastError(), L"Remote RPC host did not become ready.");
        StopRemoteHost();
        return false;
    }

    if (!ConnectToHost())
    {
        VTRACE(L"[RPC] ConnectToHost failed. error={} message={}", GetLastError(), TranslateError());
        AppendClientMetricsLog(std::format(L"[RPC] ConnectToHost failed error={} message={}",
            GetLastError(),
            TranslateError()));
        NotifyTransportFailure(GetLastError(), L"Failed to connect to remote RPC pipe.");
        StopRemoteHost();
        return false;
    }

    VTRACE(L"[RPC] EnsureConnected connected successfully. pipe={}", m_pipeName);
    AppendClientMetricsLog(std::format(L"[RPC] EnsureConnected connected pipe={}", m_pipeName));
    StartInboundProcessorIfConfigured();
    m_readerRunning.store(true, std::memory_order_release);
    m_readerThread.emplace([this]()
    {
        ReaderLoop();
    });
    return true;
}

bool NamedPipeRpcClient::EnsureHostRunning()
{
    if (m_processInfo.hProcess != nullptr)
    {
        VTRACE(L"[RPC] EnsureHostRunning reused existing process. pid={}", GetProcessId(m_processInfo.hProcess));
        return true;
    }

    const std::wstring hostExecutablePath = GetRemoteHostExecutablePath();
    m_hostLogFilePath = CreateHostLogFilePath();
    std::wstring commandLine = std::format(L"\"{}\" --pipe \"{}\" --log-file \"{}\"",
        hostExecutablePath, m_pipeName, m_hostLogFilePath);
    if (m_verboseLogging)
        commandLine += L" --log-level verbose";
    VTRACE(L"[RPC] Scan host log file. path={}", m_hostLogFilePath);
    VTRACE(L"[RPC] Launching scan host. executable={} commandLine={}", hostExecutablePath, commandLine);
    AppendClientMetricsLog(std::format(L"[RPC] Launching scan host executable={} commandLine={}",
        hostExecutablePath,
        commandLine));

    STARTUPINFO startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcess(hostExecutablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
    if (!created)
    {
        VTRACE(L"[RPC] CreateProcess failed for scan host. executable={} error={} message={}",
            hostExecutablePath, GetLastError(), TranslateError());
        AppendClientMetricsLog(std::format(L"[RPC] CreateProcess failed executable={} error={} message={}",
            hostExecutablePath,
            GetLastError(),
            TranslateError()));
        return false;
    }

    VTRACE(L"[RPC] Scan host launched. pid={} pipe={}", processInfo.dwProcessId, m_pipeName);
    AppendClientMetricsLog(std::format(L"[RPC] Scan host launched pid={} pipe={} log={}",
        processInfo.dwProcessId,
        m_pipeName,
        m_hostLogFilePath));
    m_processInfo = processInfo;
    return true;
}

bool NamedPipeRpcClient::WaitUntilHostReady()
{
    VTRACE(L"[RPC] Waiting for host readiness / pipe availability. pipe={}", m_pipeName);

    const ULONGLONG started = GetTickCount64();
    DWORD lastError = ERROR_SUCCESS;
    while (GetTickCount64() - started < kHostReadinessTimeoutMs)
    {
        if (WaitNamedPipe(m_pipeName.c_str(), 100))
            return true;

        lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_SEM_TIMEOUT)
        {
            Sleep(50);
            continue;
        }

        break;
    }

    SetLastError(lastError);
    VTRACE(L"[RPC] Host readiness timeout waiting for pipe. pipe={} lastError={} message={}",
        m_pipeName, lastError, TranslateError(lastError));
    return false;
}

bool NamedPipeRpcClient::ConnectToHost()
{
    VTRACE(L"[RPC] Connecting to scan host pipe. pipe={}", m_pipeName);

    HANDLE pipe = CreateFile(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        VTRACE(L"[RPC] Connection failed after host readiness. pipe={} error={} message={}",
            m_pipeName, GetLastError(), TranslateError());
        return false;
    }

    m_pipe = pipe;
    VTRACE(L"[RPC] Connected to scan host pipe. pipe={}", m_pipeName);
    return true;
}

bool NamedPipeRpcClient::SendJsonMessage(const std::string& json)
{
    DWORD errorCode = ERROR_SUCCESS;
    std::scoped_lock lock(m_ioMutex);
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    const DWORD length = static_cast<DWORD>(json.size());
    if (!WriteAll(m_pipe, &length, sizeof(length)) || (length != 0 && !WriteAll(m_pipe, json.data(), length)))
    {
        errorCode = GetLastError();
    }
    else
    {
        return true;
    }

    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    NotifyTransportFailure(errorCode, L"Failed to write RPC message to named pipe.");
    return false;
}

std::optional<std::string> NamedPipeRpcClient::ReadJsonMessage(const bool notifyFailure)
{
    DWORD errorCode = ERROR_SUCCESS;
    std::wstring errorMessage;
    std::scoped_lock lock(m_ioMutex);
    if (m_pipe == INVALID_HANDLE_VALUE)
        return std::nullopt;

    DWORD length = 0;
    if (!ReadAll(m_pipe, &length, sizeof(length)))
    {
        errorCode = GetLastError();
        errorMessage = L"Failed to read RPC message header from named pipe.";
    }
    else
    {
        std::string json(length, '\0');
        if (length != 0 && !ReadAll(m_pipe, json.data(), length))
        {
            errorCode = GetLastError();
            errorMessage = L"Failed to read RPC message body from named pipe.";
        }
        else
        {
            return json;
        }
    }

    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    if (notifyFailure)
    {
        NotifyTransportFailure(errorCode, errorMessage);
    }
    else
    {
        m_lastReadErrorCode = errorCode;
        m_lastReadErrorMessage = errorMessage;
    }
    return std::nullopt;
}

void NamedPipeRpcClient::ReaderLoop()
{
    VTRACE(L"[RPC] ReaderLoop started. pipe={}", m_pipeName);
    AppendClientMetricsLog(std::format(L"[RPC] ReaderLoop started pipe={}", m_pipeName));
    m_lastReadErrorCode = ERROR_SUCCESS;
    m_lastReadErrorMessage.clear();
    while (m_readerRunning.load(std::memory_order_acquire))
    {
        const auto jsonOpt = ReadJsonMessage(m_inboundQueueCapacity == 0);
        if (!jsonOpt.has_value())
            break;

        if (m_inboundQueueCapacity != 0)
        {
            if (!EnqueueInboundFrame(std::move(jsonOpt.value())))
                break;
            continue;
        }

        const auto eventOpt = TryDeserializeRpcEventMessage(jsonOpt.value());
        if (eventOpt.has_value())
            DispatchEvent(eventOpt.value());
    }

    m_readerRunning.store(false, std::memory_order_release);
    if (m_inboundQueueCapacity != 0)
    {
        SignalInboundProcessorStop();
        WaitForInboundProcessorDrain();
        TraceInboundQueueSummary();
        if (m_lastReadErrorCode != ERROR_SUCCESS && !m_shutdownRequested.load(std::memory_order_acquire))
            NotifyTransportFailure(m_lastReadErrorCode, m_lastReadErrorMessage);
    }
    if (m_verboseLogging)
        VTRACE(L"[RPC] ReaderLoop stopped. pipe={}", m_pipeName);
    AppendClientMetricsLog(std::format(L"[RPC] ReaderLoop stopped pipe={}", m_pipeName));
}

void NamedPipeRpcClient::ProcessorLoop()
{
    VTRACE(L"[RPC] Inbound processor started. capacity={}", m_inboundQueueCapacity);
    while (true)
    {
        const auto frameOpt = TakeInboundFrame();
        if (!frameOpt.has_value())
            break;

        const std::uint64_t started = ReadPerformanceCounter();
        const auto eventOpt = TryDeserializeRpcEventMessage(frameOpt->json);
        const std::uint64_t elapsed = ReadPerformanceCounter() - started;
        const bool terminalEvent = eventOpt.has_value() && IsTerminalEvent(eventOpt.value());

        {
            std::scoped_lock lock(m_inboundQueueMutex);
            ++m_inboundStats.messagesProcessed;
            m_inboundStats.processingTicks += elapsed;
        }

        if (eventOpt.has_value())
        {
            if (terminalEvent)
            {
                // Contract: the processor is single-threaded FIFO, so a terminal
                // event is observed only after all prior progress frames. Quiet
                // CSV mode exits during terminal dispatch, so emit metrics first.
                TraceInboundQueueSummary();
            }
            DispatchEvent(eventOpt.value());
        }
    }

    {
        std::scoped_lock lock(m_inboundQueueMutex);
        m_inboundProcessorStopped = true;
    }
    m_inboundDrained.notify_all();
    m_inboundCanPush.notify_all();
    VTRACE(L"[RPC] Inbound processor stopped. pipe={}", m_pipeName);
}

bool NamedPipeRpcClient::EnqueueInboundFrame(std::string json)
{
    const std::uint64_t started = ReadPerformanceCounter();
    std::unique_lock lock(m_inboundQueueMutex);
    m_inboundCanPush.wait(lock, [&]
    {
        return m_inboundStopping || m_inboundQueue.size() < m_inboundQueueCapacity;
    });
    const std::uint64_t elapsed = ReadPerformanceCounter() - started;

    if (m_inboundStopping)
        return false;

    m_inboundQueue.push_back(InboundFrame{ std::move(json) });
    ++m_inboundStats.messagesEnqueued;
    m_inboundStats.enqueueWaitTicks += elapsed;
    m_inboundStats.maxDepth = (std::max)(m_inboundStats.maxDepth, static_cast<std::uint64_t>(m_inboundQueue.size()));
    lock.unlock();
    m_inboundCanPop.notify_one();
    return true;
}

std::optional<NamedPipeRpcClient::InboundFrame> NamedPipeRpcClient::TakeInboundFrame()
{
    std::unique_lock lock(m_inboundQueueMutex);
    m_inboundCanPop.wait(lock, [&]
    {
        return m_inboundStopping || !m_inboundQueue.empty();
    });

    if (m_inboundQueue.empty())
        return std::nullopt;

    InboundFrame frame = std::move(m_inboundQueue.front());
    m_inboundQueue.pop_front();
    if (m_inboundQueue.empty())
        m_inboundDrained.notify_all();
    lock.unlock();
    m_inboundCanPush.notify_one();
    return frame;
}

void NamedPipeRpcClient::StartInboundProcessorIfConfigured()
{
    if (m_inboundQueueCapacity == 0 || m_processorThread.has_value())
        return;

    {
        std::scoped_lock lock(m_inboundQueueMutex);
        m_inboundStopping = false;
        m_inboundProcessorStopped = false;
        m_inboundQueue.clear();
        m_inboundStats = {};
    }

    // Experimental: inbound queueing is disabled by default and exists only
    // behind WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY. The current performance
    // baseline is the synchronous reader path because it gives the real app the
    // best scan/observer overlap measured so far.
    // Contract: queued inbound mode preserves wire order by using exactly one
    // processor. The reader only extracts frames; parsing and observer dispatch
    // remain on the same ordered path used by synchronous mode.
    m_processorThread.emplace([this]()
    {
        ProcessorLoop();
    });
}

void NamedPipeRpcClient::SignalInboundProcessorStop()
{
    {
        std::scoped_lock lock(m_inboundQueueMutex);
        m_inboundStopping = true;
    }
    m_inboundCanPop.notify_all();
    m_inboundCanPush.notify_all();
}

void NamedPipeRpcClient::WaitForInboundProcessorDrain()
{
    if (m_inboundQueueCapacity == 0)
        return;

    std::unique_lock lock(m_inboundQueueMutex);
    m_inboundDrained.wait(lock, [&]
    {
        return m_inboundProcessorStopped;
    });
}

void NamedPipeRpcClient::StopInboundProcessor()
{
    SignalInboundProcessorStop();
    if (m_processorThread.has_value() && m_processorThread->joinable())
        m_processorThread->join();
    m_processorThread.reset();
}

void NamedPipeRpcClient::TraceInboundQueueSummary() const
{
    if (m_inboundQueueCapacity == 0)
        return;

    InboundQueueStats stats{};
    {
        std::scoped_lock lock(m_inboundQueueMutex);
        stats = m_inboundStats;
    }

    const std::wstring line = std::format(L"[RPC] InboundQueueSummary capacity={} enqueued={} processed={} maxDepth={} enqueueWaitMs={:.3f} processingMs={:.3f}",
        m_inboundQueueCapacity,
        stats.messagesEnqueued,
        stats.messagesProcessed,
        stats.maxDepth,
        PerformanceTicksToMilliseconds(stats.enqueueWaitTicks),
        PerformanceTicksToMilliseconds(stats.processingTicks));
    VTRACE(L"{}", line);
    AppendClientMetricsLog(line);
}

void NamedPipeRpcClient::AppendClientMetricsLog(const std::wstring_view line) const
{
    if (m_clientMetricsLogFilePath.empty())
        return;

    std::ofstream stream(m_clientMetricsLogFilePath, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream.is_open())
        return;

    const int length = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return;

    std::string utf8(length, '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), length, nullptr, nullptr);
    stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    stream.write("\n", 1);
}

void NamedPipeRpcClient::NotifyTransportFailure(const unsigned long errorCode, const std::wstring& message)
{
    VTRACE(L"[RPC] Transport failure notified. error={} message={}", errorCode, message);
    StopRemoteHost();

    if (m_shutdownRequested.load(std::memory_order_acquire))
        return;

    if (m_transportFailureNotified.exchange(true, std::memory_order_acq_rel))
        return;

    IRpcTransportEventHandler* handler = nullptr;
    {
        std::scoped_lock lock(m_handlerMutex);
        handler = m_handler;
    }

    if (handler != nullptr)
        handler->OnTransportFailure(errorCode, message);
}

void NamedPipeRpcClient::ClosePipe()
{
    std::scoped_lock lock(m_ioMutex);
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

void NamedPipeRpcClient::StopRemoteHost()
{
    if (m_processInfo.hProcess == nullptr)
        return;

    const DWORD processId = m_processInfo.dwProcessId;
    (void)WaitForSingleObject(m_processInfo.hProcess, 2000);
    CloseHandle(m_processInfo.hThread);
    CloseHandle(m_processInfo.hProcess);
    m_processInfo = {};
    VTRACE(L"[RPC] Scan host process handles closed. pid={}", processId);
}

void NamedPipeRpcClient::DispatchEvent(const RpcEventMessage& event)
{
    IRpcTransportEventHandler* handler = nullptr;
    const bool verboseLogging = m_verboseLogging;
    {
        std::scoped_lock lock(m_handlerMutex);
        handler = m_handler;
    }

    if (handler == nullptr)
        return;

    std::visit([handler, verboseLogging](const auto& typedEvent)
    {
        using TEvent = std::decay_t<decltype(typedEvent)>;
        if constexpr (std::is_same_v<TEvent, RpcDirectoryProgressEvent>)
        {
            if (verboseLogging)
            {
                VTRACE(L"[RPC] Received DirectoryProgress. request={} path=\"{}\" finished={} files={} directories={}",
                    typedEvent.requestId, typedEvent.directoryPath, typedEvent.finished,
                    typedEvent.files.size(), typedEvent.directories.size());
            }
            handler->OnRemoteDirectoryProgress(typedEvent);
        }
        else if constexpr (std::is_same_v<TEvent, RpcDirectoryProgressBatchEvent>)
        {
            VTRACE(L"[RPC] Received DirectoryProgressBatch. request={} items={}",
                typedEvent.requestId, typedEvent.items.size());
            for (const RpcDirectoryProgressEvent& item : typedEvent.items)
            {
                if (verboseLogging)
                {
                    VTRACE(L"[RPC] Replaying DirectoryProgress from batch. request={} path=\"{}\" finished={} files={} directories={}",
                        item.requestId, item.directoryPath, item.finished,
                        item.files.size(), item.directories.size());
                }
                handler->OnRemoteDirectoryProgress(item);
            }
        }
        else if constexpr (std::is_same_v<TEvent, RpcScanCompletedEvent>)
        {
            VTRACE(L"[RPC] Received ScanCompleted. request={}", typedEvent.requestId);
            handler->OnRemoteScanCompleted(typedEvent);
        }
        else if constexpr (std::is_same_v<TEvent, RpcScanCanceledEvent>)
        {
            VTRACE(L"[RPC] Received ScanCanceled. request={} reason={}",
                typedEvent.requestId, static_cast<int>(typedEvent.reason));
            handler->OnRemoteScanCanceled(typedEvent);
        }
        else
        {
            VTRACE(L"[RPC] Received ScanFailed. request={} path=\"{}\" errorCode={} message=\"{}\"",
                typedEvent.requestId, typedEvent.path, typedEvent.errorCode, typedEvent.message);
            handler->OnRemoteScanFailed(typedEvent);
        }
    }, event);
}

bool NamedPipeRpcClient::IsVerboseLoggingEnabled()
{
    return IsRpcVerboseLoggingEnabledFromCommandLine();
}

std::size_t NamedPipeRpcClient::GetInboundQueueCapacity()
{
    // Experimental performance path: release builds do not enable the inbound
    // queue implicitly. A positive WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY is the
    // only activation mechanism; zero keeps the synchronous baseline path.
    return ReadInboundQueueCapacityFromEnvironment();
}

std::wstring NamedPipeRpcClient::GetClientMetricsLogFilePath()
{
    const DWORD length = GetEnvironmentVariableW(L"WINDIRSTAT_RPC_CLIENT_LOG_FILE", nullptr, 0);
    if (length == 0)
        return {};

    std::wstring value(length, L'\0');
    const DWORD actualLength = GetEnvironmentVariableW(L"WINDIRSTAT_RPC_CLIENT_LOG_FILE", value.data(), length);
    if (actualLength == 0 || actualLength >= length)
        return {};

    value.resize(actualLength);
    return value;
}

std::wstring NamedPipeRpcClient::CreatePipeName()
{
    GUID guid{};
    (void)CoCreateGuid(&guid);

    std::array<wchar_t, 64> guidText{};
    (void)StringFromGUID2(guid, guidText.data(), static_cast<int>(guidText.size()));

    std::wstring pipeName = LR"(\\.\pipe\WinDirStat.RpcScan.)";
    pipeName += guidText.data();
    pipeName.erase(std::remove(pipeName.begin(), pipeName.end(), L'{'), pipeName.end());
    pipeName.erase(std::remove(pipeName.begin(), pipeName.end(), L'}'), pipeName.end());
    return pipeName;
}

std::wstring NamedPipeRpcClient::CreateHostLogFilePath()
{
    std::array<wchar_t, MAX_PATH> tempPath{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
    const std::filesystem::path directory = (length == 0 || length >= tempPath.size())
        ? std::filesystem::temp_directory_path()
        : std::filesystem::path(std::wstring(tempPath.data(), length));

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::wstring fileName = std::format(L"windirstat-scan-host-{}-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.log",
        GetCurrentProcessId(),
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return (directory / fileName).wstring();
}

std::wstring NamedPipeRpcClient::GetRemoteHostExecutablePath()
{
    const std::filesystem::path appPath = GetAppFileName();
    const std::wstring appStem = appPath.stem().wstring();
    std::wstring suffix;

    constexpr std::wstring_view appPrefix = L"WinDirStat";
    if (appStem.starts_with(appPrefix))
        suffix = appStem.substr(appPrefix.size());

    const std::wstring hostPath = (appPath.parent_path() /
        std::filesystem::path(std::wstring(L"windirstat-scan-host") + suffix + appPath.extension().wstring())).wstring();
    VTRACE(L"[RPC] Resolved scan host path. app={} host={}", appPath.wstring(), hostPath);
    return hostPath;
}
