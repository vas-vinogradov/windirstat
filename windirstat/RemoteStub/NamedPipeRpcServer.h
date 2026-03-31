#pragma once

#include "Engine/Rpc/RpcMessages.h"

#include <optional>
#include <string>

class NamedPipeRpcServer
{
public:
    explicit NamedPipeRpcServer(std::wstring pipeName);
    ~NamedPipeRpcServer();

    bool Listen();
    std::optional<RpcRequestMessage> ReadRequestMessage();
    std::optional<RpcRequestMessage> ReadRequestMessage(DWORD timeoutMs);
    bool SendEventMessage(const RpcEventMessage& message);

private:
    bool WriteJsonMessage(const std::string& json);
    std::optional<std::string> ReadJsonMessage();
    void Close();

    std::wstring m_pipeName;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
};
