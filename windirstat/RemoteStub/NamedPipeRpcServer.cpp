#include "pch.h"
#include "RemoteStub/NamedPipeRpcServer.h"

namespace
{
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

NamedPipeRpcServer::NamedPipeRpcServer(std::wstring pipeName)
    : m_pipeName(std::move(pipeName))
{
}

NamedPipeRpcServer::~NamedPipeRpcServer()
{
    Close();
}

bool NamedPipeRpcServer::Listen()
{
    m_pipe = CreateNamedPipe(m_pipeName.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    const BOOL connected = ConnectNamedPipe(m_pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
    return connected == TRUE;
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

bool NamedPipeRpcServer::SendEventMessage(const RpcEventMessage& message)
{
    return WriteJsonMessage(SerializeRpcEventMessage(message));
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
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}
