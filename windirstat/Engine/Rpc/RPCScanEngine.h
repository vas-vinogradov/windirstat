#pragma once

#include "Engine/Rpc/IRpcTransportClient.h"
#include "IScanEngine.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

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
    bool OwnsTraversal() const override { return true; }
    std::uint64_t GetActiveRequestId() const override;

private:
    struct PendingStart
    {
        ScanRequest request{};
        IScanObserver* observer = nullptr;
        std::uint64_t requestId = 0;
    };

    void OnRemoteDirectoryProgress(const RpcDirectoryProgressEvent& event) override;
    void OnRemoteScanCompleted(const RpcScanCompletedEvent& event) override;
    void OnRemoteScanCanceled(const RpcScanCanceledEvent& event) override;
    void OnRemoteScanFailed(const RpcScanFailedEvent& event) override;
    void OnTransportFailure(unsigned long errorCode, const std::wstring& message) override;

    void ActivateRequest(IScanObserver& observer, std::uint64_t requestId);
    void StartPendingRequestIfAny();
    void ResetRequestLifecycle();
    void TryCloseRequestInput(std::uint64_t requestId);
    void FailActiveRequestForTransport(unsigned long errorCode, const std::wstring& message);
    bool IsCurrentRequest(std::uint64_t requestId) const;
    IScanObserver* GetActiveObserver() const;
    PendingStart TakePendingStart();
    void RecordExternalInputPath(const std::wstring& path);
    void UndoExternalInputPath(const std::wstring& path);
    bool CompleteExternalInputPath(const std::wstring& path);
    void ResetExternalInputTracking();
    std::wstring DescribeExternalInputState() const;
    std::wstring DescribeExternalInputStateLocked() const;

    std::unique_ptr<IRpcTransportClient> m_transportClient;
    mutable std::mutex m_stateMutex;
    mutable std::mutex m_observerMutex;
    mutable std::mutex m_inputTrackingMutex;
    IScanObserver* m_activeObserver = nullptr;
    std::optional<PendingStart> m_pendingStart;
    std::unordered_map<std::wstring, std::uint32_t> m_pendingInputPaths;
    std::atomic_uint64_t m_nextRequestId = 1;
    std::atomic_uint64_t m_activeRequestId = 0;
    std::atomic_uint32_t m_outstandingWorkItems = 0;
    std::atomic_bool m_requestInputClosed = false;
    std::atomic_bool m_cancelRequested = false;
    std::atomic_bool m_terminalResolved = false;
    std::atomic_bool m_running = false;
};
