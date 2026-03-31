#pragma once

#include "Engine/Rpc/IRpcTransportClient.h"

#include <atomic>
#include <mutex>
#include <optional>
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
    bool EnsureConnected();
    bool StartRemoteHost();
    bool ConnectPipe();
    bool SendJsonMessage(const std::string& json);
    std::optional<std::string> ReadJsonMessage();
    void ReaderLoop();
    void ClosePipe();
    void StopRemoteHost();
    void DispatchEvent(const RpcEventMessage& event);
    static std::wstring CreatePipeName();

    std::wstring m_pipeName;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION m_processInfo{};
    mutable std::mutex m_ioMutex;
    mutable std::mutex m_connectMutex;
    mutable std::mutex m_handlerMutex;
    IRpcTransportEventHandler* m_handler = nullptr;
    std::optional<std::jthread> m_readerThread;
    std::atomic_bool m_readerRunning = false;
};
