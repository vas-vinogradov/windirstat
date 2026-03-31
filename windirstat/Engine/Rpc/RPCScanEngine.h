#pragma once

#include "Engine/Rpc/IRpcTransportClient.h"
#include "IScanEngine.h"

#include <atomic>
#include <memory>
#include <mutex>

class RPCScanEngine final
    : public IScanEngine
    , private IRpcTransportEventHandler
{
public:
    explicit RPCScanEngine(std::unique_ptr<IRpcTransportClient> transportClient);
    ~RPCScanEngine() override;

    void StartScan(const ScanRequest& request, IScanObserver& observer) override;
    void Enqueue(const ScanRequest& request) override;
    void Cancel(ScanTerminalReason reason = ScanTerminalReason::EngineInterrupted) override;
    void Suspend() override;
    void Resume() override;
    bool IsRunning() const override;
    std::uint64_t GetActiveRequestId() const override;

private:
    void OnRemoteDirectoryProgress(const RpcDirectoryProgressEvent& event) override;
    void OnRemoteScanCompleted(const RpcScanCompletedEvent& event) override;
    void OnRemoteScanCanceled(const RpcScanCanceledEvent& event) override;
    void OnRemoteScanFailed(const RpcScanFailedEvent& event) override;

    void ResetRequestLifecycle();
    void TryCloseRequestInput(std::uint64_t requestId);
    bool IsCurrentRequest(std::uint64_t requestId) const;
    IScanObserver* GetObserver() const;

    std::unique_ptr<IRpcTransportClient> m_transportClient;
    mutable std::mutex m_observerMutex;
    IScanObserver* m_observer = nullptr;
    std::atomic_uint64_t m_nextRequestId = 1;
    std::atomic_uint64_t m_activeRequestId = 0;
    std::atomic_uint32_t m_outstandingWorkItems = 0;
    std::atomic_bool m_requestInputClosed = false;
    std::atomic_bool m_cancelRequested = false;
    std::atomic_bool m_running = false;
};
