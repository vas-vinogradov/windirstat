#include "pch.h"
#include "Engine/Rpc/NamedPipeRpcClient.h"

#include "HelpersInterface.h"

namespace
{
constexpr DWORD kConnectTimeoutMs = 5000;

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
}

NamedPipeRpcClient::NamedPipeRpcClient()
    : m_pipeName(CreatePipeName())
{
}

NamedPipeRpcClient::~NamedPipeRpcClient()
{
    m_shutdownRequested.store(true, std::memory_order_release);
    m_readerRunning.store(false, std::memory_order_release);
    ClosePipe();

    if (m_readerThread.has_value() && m_readerThread->joinable())
        m_readerThread->join();
    m_readerThread.reset();

    StopRemoteHost();
}

void NamedPipeRpcClient::SetEventHandler(IRpcTransportEventHandler* handler)
{
    std::scoped_lock lock(m_handlerMutex);
    m_handler = handler;
}

bool NamedPipeRpcClient::SendStartScan(const RpcStartScanRequest& request)
{
    return EnsureConnected() && SendJsonMessage(SerializeRpcRequestMessage(RpcRequestMessage(request)));
}

bool NamedPipeRpcClient::SendEnqueue(const RpcEnqueueRequest& request)
{
    return EnsureConnected() && SendJsonMessage(SerializeRpcRequestMessage(RpcRequestMessage(request)));
}

bool NamedPipeRpcClient::SendCloseRequestInput(const RpcCloseRequestInput& request)
{
    return EnsureConnected() && SendJsonMessage(SerializeRpcRequestMessage(RpcRequestMessage(request)));
}

bool NamedPipeRpcClient::SendCancelScan(const RpcCancelScanRequest& request)
{
    return EnsureConnected() && SendJsonMessage(SerializeRpcRequestMessage(RpcRequestMessage(request)));
}

bool NamedPipeRpcClient::EnsureConnected()
{
    std::scoped_lock connectLock(m_connectMutex);
    if (m_pipe != INVALID_HANDLE_VALUE)
        return true;

    m_transportFailureNotified.store(false, std::memory_order_release);

    if (!StartRemoteHost())
    {
        NotifyTransportFailure(GetLastError(), L"Failed to start remote RPC host.");
        return false;
    }

    if (!ConnectPipe())
    {
        NotifyTransportFailure(GetLastError(), L"Failed to connect to remote RPC pipe.");
        StopRemoteHost();
        return false;
    }

    m_readerRunning.store(true, std::memory_order_release);
    m_readerThread.emplace([this]()
    {
        ReaderLoop();
    });
    return true;
}

bool NamedPipeRpcClient::StartRemoteHost()
{
    if (m_processInfo.hProcess != nullptr)
        return true;

    std::wstring commandLine = std::format(L"\"{}\" --remote-stub --pipe \"{}\"", GetAppFileName(), m_pipeName);

    STARTUPINFO startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcess(GetAppFileName().c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
    if (!created)
        return false;

    m_processInfo = processInfo;
    return true;
}

bool NamedPipeRpcClient::ConnectPipe()
{
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < kConnectTimeoutMs)
    {
        HANDLE pipe = CreateFile(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            m_pipe = pipe;
            return true;
        }

        if (!WaitNamedPipe(m_pipeName.c_str(), 100))
            Sleep(50);
    }

    return false;
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

std::optional<std::string> NamedPipeRpcClient::ReadJsonMessage()
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

    NotifyTransportFailure(errorCode, errorMessage);
    return std::nullopt;
}

void NamedPipeRpcClient::ReaderLoop()
{
    while (m_readerRunning.load(std::memory_order_acquire))
    {
        const auto jsonOpt = ReadJsonMessage();
        if (!jsonOpt.has_value())
            break;

        const auto eventOpt = TryDeserializeRpcEventMessage(jsonOpt.value());
        if (eventOpt.has_value())
            DispatchEvent(eventOpt.value());
    }

    m_readerRunning.store(false, std::memory_order_release);
}

void NamedPipeRpcClient::NotifyTransportFailure(const unsigned long errorCode, const std::wstring& message)
{
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

    (void)WaitForSingleObject(m_processInfo.hProcess, 2000);
    CloseHandle(m_processInfo.hThread);
    CloseHandle(m_processInfo.hProcess);
    m_processInfo = {};
}

void NamedPipeRpcClient::DispatchEvent(const RpcEventMessage& event)
{
    IRpcTransportEventHandler* handler = nullptr;
    {
        std::scoped_lock lock(m_handlerMutex);
        handler = m_handler;
    }

    if (handler == nullptr)
        return;

    std::visit([handler](const auto& typedEvent)
    {
        using TEvent = std::decay_t<decltype(typedEvent)>;
        if constexpr (std::is_same_v<TEvent, RpcDirectoryProgressEvent>)
            handler->OnRemoteDirectoryProgress(typedEvent);
        else if constexpr (std::is_same_v<TEvent, RpcScanCompletedEvent>)
            handler->OnRemoteScanCompleted(typedEvent);
        else if constexpr (std::is_same_v<TEvent, RpcScanCanceledEvent>)
            handler->OnRemoteScanCanceled(typedEvent);
        else
            handler->OnRemoteScanFailed(typedEvent);
    }, event);
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
