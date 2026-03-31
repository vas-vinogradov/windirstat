#include "pch.h"
#include "Engine/Rpc/RPCScanEngine.h"

#include "DirectoryProgressBatch.h"
#include "IScanObserver.h"
#include "ScanError.h"

RPCScanEngine::RPCScanEngine(std::unique_ptr<IRpcTransportClient> transportClient)
    : m_transportClient(std::move(transportClient))
{
    ASSERT(m_transportClient != nullptr);
    if (m_transportClient != nullptr)
        m_transportClient->SetEventHandler(this);
}

RPCScanEngine::~RPCScanEngine()
{
    Cancel(ScanTerminalReason::EngineInterrupted);
    if (m_transportClient != nullptr)
        m_transportClient->SetEventHandler(nullptr);
}

void RPCScanEngine::StartScan(const ScanRequest& request, IScanObserver& observer)
{
    {
        std::scoped_lock lock(m_observerMutex);
        m_observer = &observer;
    }

    const std::uint64_t requestId = m_nextRequestId.fetch_add(1, std::memory_order_acq_rel);
    m_activeRequestId.store(requestId, std::memory_order_release);
    m_outstandingWorkItems.store(1, std::memory_order_release);
    m_requestInputClosed.store(false, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);

    RpcStartScanRequest startRequest{};
    startRequest.requestId = requestId;
    startRequest.rootPath = request.rootPath;

    if (m_transportClient != nullptr && m_transportClient->SendStartScan(startRequest))
        return;

    ResetRequestLifecycle();

    ScanError error{};
    error.requestId = requestId;
    error.path = request.rootPath;
    error.message = L"Failed to send StartScan over RPC transport.";
    error.errorCode = ERROR_BROKEN_PIPE;
    observer.OnError(error);
    observer.OnCanceled(requestId, ScanTerminalReason::EngineInterrupted);
}

void RPCScanEngine::Enqueue(const ScanRequest& request)
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    const std::uint64_t activeRequestId = m_activeRequestId.load(std::memory_order_acquire);
    if (activeRequestId == 0)
        return;

    if (request.requestId != 0 && request.requestId != activeRequestId)
        return;

    if (m_cancelRequested.load(std::memory_order_acquire))
        return;

    m_outstandingWorkItems.fetch_add(1, std::memory_order_acq_rel);

    RpcEnqueueRequest enqueueRequest{};
    enqueueRequest.requestId = activeRequestId;
    enqueueRequest.rootPath = request.rootPath;

    if (m_transportClient != nullptr)
    {
        if (m_transportClient->SendEnqueue(enqueueRequest))
            return;
    }

    m_outstandingWorkItems.fetch_sub(1, std::memory_order_acq_rel);
}

void RPCScanEngine::Cancel(const ScanTerminalReason reason)
{
    if (!m_running.exchange(false))
        return;

    m_cancelRequested.store(true, std::memory_order_release);
    m_requestInputClosed.store(true, std::memory_order_release);

    RpcCancelScanRequest cancelRequest{};
    cancelRequest.requestId = m_activeRequestId.load(std::memory_order_acquire);
    cancelRequest.reason = reason;

    if (m_transportClient != nullptr)
        (void)m_transportClient->SendCancelScan(cancelRequest);
}

void RPCScanEngine::Suspend()
{
}

void RPCScanEngine::Resume()
{
}

bool RPCScanEngine::IsRunning() const
{
    return m_running.load(std::memory_order_acquire);
}

std::uint64_t RPCScanEngine::GetActiveRequestId() const
{
    return m_activeRequestId.load(std::memory_order_acquire);
}

void RPCScanEngine::OnRemoteDirectoryProgress(const RpcDirectoryProgressEvent& event)
{
    if (!IsCurrentRequest(event.requestId))
        return;

    IScanObserver* const observer = GetObserver();
    if (observer == nullptr)
        return;

    DirectoryProgressBatch batch{};
    batch.requestId = event.requestId;
    batch.directoryPath = event.directoryPath;
    batch.files = event.files;
    batch.directories = event.directories;
    batch.finished = event.finished;
    observer->OnDirectoryProgress(std::move(batch));

    if (event.finished)
    {
        const std::uint32_t current = m_outstandingWorkItems.load(std::memory_order_acquire);
        if (current == 0)
            return;

        const std::uint32_t remaining = m_outstandingWorkItems.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && !m_cancelRequested.load(std::memory_order_acquire))
            TryCloseRequestInput(event.requestId);
    }
}

void RPCScanEngine::OnRemoteScanCompleted(const RpcScanCompletedEvent& event)
{
    if (!IsCurrentRequest(event.requestId))
        return;

    ResetRequestLifecycle();

    IScanObserver* const observer = GetObserver();
    if (observer != nullptr)
        observer->OnCompleted(event.requestId);
}

void RPCScanEngine::OnRemoteScanCanceled(const RpcScanCanceledEvent& event)
{
    if (!IsCurrentRequest(event.requestId))
        return;

    ResetRequestLifecycle();

    IScanObserver* const observer = GetObserver();
    if (observer != nullptr)
        observer->OnCanceled(event.requestId, event.reason);
}

void RPCScanEngine::OnRemoteScanFailed(const RpcScanFailedEvent& event)
{
    if (!IsCurrentRequest(event.requestId))
        return;

    ResetRequestLifecycle();

    IScanObserver* const observer = GetObserver();
    if (observer == nullptr)
        return;

    ScanError error{};
    error.requestId = event.requestId;
    error.path = event.path;
    error.message = event.message;
    error.errorCode = event.errorCode;
    observer->OnError(error);
    observer->OnCanceled(event.requestId, ScanTerminalReason::EngineInterrupted);
}

bool RPCScanEngine::IsCurrentRequest(const std::uint64_t requestId) const
{
    return requestId != 0 && requestId == m_activeRequestId.load(std::memory_order_acquire);
}

IScanObserver* RPCScanEngine::GetObserver() const
{
    std::scoped_lock lock(m_observerMutex);
    return m_observer;
}

void RPCScanEngine::ResetRequestLifecycle()
{
    m_outstandingWorkItems.store(0, std::memory_order_release);
    m_requestInputClosed.store(true, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
}

void RPCScanEngine::TryCloseRequestInput(const std::uint64_t requestId)
{
    if (!IsCurrentRequest(requestId))
        return;

    if (m_requestInputClosed.exchange(true, std::memory_order_acq_rel))
        return;

    RpcCloseRequestInput closeRequest{};
    closeRequest.requestId = requestId;
    if (m_transportClient != nullptr)
        (void)m_transportClient->SendCloseRequestInput(closeRequest);
}
