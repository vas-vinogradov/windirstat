#include "pch.h"
#include "Engine/Rpc/RPCScanEngine.h"

#include "DirectoryProgressBatch.h"
#include "IScanObserver.h"
#include "ScanError.h"

namespace
{
std::wstring RpcTimestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

void RpcClientLog(std::uint64_t requestId, std::wstring_view event, std::wstring_view details = {})
{
    std::wstring line = std::format(L"[RPC][CLIENT] ts={} request={} event={}",
        RpcTimestamp(), requestId, event);
    if (!details.empty())
        line += std::format(L" {}", details);

    line += L"\n";
    OutputDebugStringW(line.c_str());
}
}

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
    const std::uint64_t requestId = m_nextRequestId.fetch_add(1, std::memory_order_acq_rel);
    ASSERT(requestId != 0);
    std::uint64_t requestToCancel = 0;
    bool startImmediately = false;

    {
        std::scoped_lock lock(m_stateMutex);

        const std::uint64_t activeRequestId = m_activeRequestId.load(std::memory_order_acquire);
        if (activeRequestId != 0 && !m_terminalResolved.load(std::memory_order_acquire))
        {
            ASSERT(activeRequestId != 0);
            ASSERT(activeRequestId != requestId);
            ASSERT(GetActiveObserver() != nullptr);
            ASSERT(!m_requestInputClosed.load(std::memory_order_acquire));

            // One RPC session owns at most one active request. A restart first resolves
            // the old request terminally, then the queued replacement becomes active.
            m_pendingStart = PendingStart{ request, &observer, requestId };
            ASSERT(m_pendingStart->requestId != 0);
            requestToCancel = activeRequestId;
            RpcClientLog(requestId, L"ReplacementQueued",
                std::format(L"replaces={} path=\"{}\"", activeRequestId, request.rootPath));
        }
        else
        {
            ActivateRequest(observer, requestId);
            startImmediately = true;
        }
    }

    if (!startImmediately)
    {
        if (requestToCancel == 0)
            return;

        if (m_cancelRequested.exchange(true, std::memory_order_acq_rel))
            return;

        m_requestInputClosed.store(true, std::memory_order_release);

        RpcCancelScanRequest cancelRequest{};
        cancelRequest.requestId = requestToCancel;
        cancelRequest.reason = ScanTerminalReason::Restarted;
        RpcClientLog(requestToCancel, L"CancelRequested",
            std::format(L"reason={} replacement={}", static_cast<int>(cancelRequest.reason), requestId));

        if (m_transportClient != nullptr && m_transportClient->SendCancelScan(cancelRequest))
            return;

        FailActiveRequestForTransport(ERROR_BROKEN_PIPE, L"Failed to send CancelScan over RPC transport for replacement.");
        return;
    }

    RpcStartScanRequest startRequest{};
    startRequest.requestId = requestId;
    startRequest.rootPath = request.rootPath;
    RpcClientLog(requestId, L"StartScanInvoked", std::format(L"path=\"{}\"", request.rootPath));

    if (m_transportClient != nullptr && m_transportClient->SendStartScan(startRequest))
        return;

    FailActiveRequestForTransport(ERROR_BROKEN_PIPE, L"Failed to send StartScan over RPC transport.");
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

    ASSERT(activeRequestId != 0);
    ASSERT(!m_requestInputClosed.load(std::memory_order_acquire));
    ASSERT(GetActiveObserver() != nullptr);
    m_outstandingWorkItems.fetch_add(1, std::memory_order_acq_rel);

    RpcEnqueueRequest enqueueRequest{};
    enqueueRequest.requestId = activeRequestId;
    enqueueRequest.rootPath = request.rootPath;
    RpcClientLog(activeRequestId, L"EnqueueSent",
        std::format(L"path=\"{}\" outstanding={}", request.rootPath, m_outstandingWorkItems.load(std::memory_order_acquire)));

    if (m_transportClient != nullptr)
    {
        if (m_transportClient->SendEnqueue(enqueueRequest))
            return;
    }

    m_outstandingWorkItems.fetch_sub(1, std::memory_order_acq_rel);
    FailActiveRequestForTransport(ERROR_BROKEN_PIPE, L"Failed to send Enqueue over RPC transport.");
}

void RPCScanEngine::Cancel(const ScanTerminalReason reason)
{
    {
        std::scoped_lock lock(m_stateMutex);
        m_pendingStart.reset();
    }

    const std::uint64_t requestId = m_activeRequestId.load(std::memory_order_acquire);
    if (requestId == 0 || m_terminalResolved.load(std::memory_order_acquire))
        return;

    ASSERT(GetActiveObserver() != nullptr);
    if (m_cancelRequested.exchange(true, std::memory_order_acq_rel))
        return;

    m_requestInputClosed.store(true, std::memory_order_release);

    RpcCancelScanRequest cancelRequest{};
    cancelRequest.requestId = requestId;
    cancelRequest.reason = reason;
    RpcClientLog(requestId, L"CancelRequested",
        std::format(L"reason={}", static_cast<int>(reason)));

    if (m_transportClient != nullptr)
    {
        if (m_transportClient->SendCancelScan(cancelRequest))
            return;
    }

    FailActiveRequestForTransport(ERROR_BROKEN_PIPE, L"Failed to send CancelScan over RPC transport.");
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
    if (!IsCurrentRequest(event.requestId) || m_terminalResolved.load(std::memory_order_acquire))
        return;

    ASSERT(event.requestId != 0);
    ASSERT(GetActiveObserver() != nullptr);
    RpcClientLog(event.requestId, L"DirectoryProgressReceived",
        std::format(L"path=\"{}\" finished={} outstanding={}",
            event.directoryPath, event.finished ? 1 : 0,
            m_outstandingWorkItems.load(std::memory_order_acquire)));

    IScanObserver* const observer = GetActiveObserver();
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
        ASSERT(current > 0);
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

    ASSERT(event.requestId != 0);
    if (m_terminalResolved.exchange(true, std::memory_order_acq_rel))
        return;

    IScanObserver* const observer = GetActiveObserver();
    ASSERT(observer != nullptr);
    RpcClientLog(event.requestId, L"TerminalReceived", L"terminal=Completed");
    ResetRequestLifecycle();

    if (observer != nullptr)
        observer->OnCompleted(event.requestId);

    StartPendingRequestIfAny();
}

void RPCScanEngine::OnRemoteScanCanceled(const RpcScanCanceledEvent& event)
{
    if (!IsCurrentRequest(event.requestId))
        return;

    ASSERT(event.requestId != 0);
    if (m_terminalResolved.exchange(true, std::memory_order_acq_rel))
        return;

    IScanObserver* const observer = GetActiveObserver();
    ASSERT(observer != nullptr);
    RpcClientLog(event.requestId, L"TerminalReceived",
        std::format(L"terminal=Canceled reason={}", static_cast<int>(event.reason)));
    ResetRequestLifecycle();

    if (observer != nullptr)
        observer->OnCanceled(event.requestId, event.reason);

    StartPendingRequestIfAny();
}

void RPCScanEngine::OnRemoteScanFailed(const RpcScanFailedEvent& event)
{
    if (!IsCurrentRequest(event.requestId))
        return;

    ASSERT(event.requestId != 0);
    if (m_terminalResolved.exchange(true, std::memory_order_acq_rel))
        return;

    IScanObserver* const observer = GetActiveObserver();
    ASSERT(observer != nullptr);
    // Wire-level "failed" currently means error details for the active request.
    // The observer contract remains OnError(...) followed by terminal canceled.
    RpcClientLog(event.requestId, L"TerminalReceived",
        std::format(L"terminal=Failed path=\"{}\" errorCode={} message=\"{}\"",
            event.path, event.errorCode, event.message));
    ResetRequestLifecycle();

    if (observer != nullptr)
    {
        ScanError error{};
        error.requestId = event.requestId;
        error.path = event.path;
        error.message = event.message;
        error.errorCode = event.errorCode;
        observer->OnError(error);
        observer->OnCanceled(event.requestId, ScanTerminalReason::EngineInterrupted);
    }

    StartPendingRequestIfAny();
}

void RPCScanEngine::OnTransportFailure(const unsigned long errorCode, const std::wstring& message)
{
    const std::uint64_t requestId = m_activeRequestId.load(std::memory_order_acquire);
    ASSERT(requestId == 0 || GetActiveObserver() != nullptr);
    RpcClientLog(requestId, L"TransportFailure",
        std::format(L"errorCode={} message=\"{}\"", errorCode, message));
    FailActiveRequestForTransport(errorCode, message);
}

bool RPCScanEngine::IsCurrentRequest(const std::uint64_t requestId) const
{
    return requestId != 0 && requestId == m_activeRequestId.load(std::memory_order_acquire);
}

IScanObserver* RPCScanEngine::GetActiveObserver() const
{
    std::scoped_lock lock(m_observerMutex);
    return m_activeObserver;
}

void RPCScanEngine::ActivateRequest(IScanObserver& observer, const std::uint64_t requestId)
{
    ASSERT(requestId != 0);
    ASSERT(m_activeRequestId.load(std::memory_order_acquire) == 0);
    ASSERT(!m_running.load(std::memory_order_acquire));
    ASSERT(m_outstandingWorkItems.load(std::memory_order_acquire) == 0);
    ASSERT(!m_cancelRequested.load(std::memory_order_acquire));

    {
        std::scoped_lock lock(m_observerMutex);
        ASSERT(m_activeObserver == nullptr);
        m_activeObserver = &observer;
    }

    m_activeRequestId.store(requestId, std::memory_order_release);
    m_outstandingWorkItems.store(1, std::memory_order_release);
    m_requestInputClosed.store(false, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);
    m_terminalResolved.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    RpcClientLog(requestId, L"RequestActivated",
        std::format(L"outstanding={} inputClosed=0 cancelPending=0", m_outstandingWorkItems.load(std::memory_order_acquire)));
}

RPCScanEngine::PendingStart RPCScanEngine::TakePendingStart()
{
    std::scoped_lock lock(m_stateMutex);
    if (!m_pendingStart.has_value())
        return {};

    PendingStart pending = std::move(m_pendingStart.value());
    m_pendingStart.reset();
    ASSERT(pending.requestId != 0);
    ASSERT(pending.observer != nullptr);
    return pending;
}

void RPCScanEngine::StartPendingRequestIfAny()
{
    PendingStart pending = TakePendingStart();
    if (pending.requestId == 0 || pending.observer == nullptr)
        return;

    ASSERT(m_activeRequestId.load(std::memory_order_acquire) == 0);
    ASSERT(!m_running.load(std::memory_order_acquire));
    ASSERT(GetActiveObserver() == nullptr);
    RpcClientLog(pending.requestId, L"PendingRequestPromotion",
        std::format(L"path=\"{}\"", pending.request.rootPath));
    ActivateRequest(*pending.observer, pending.requestId);

    RpcStartScanRequest startRequest{};
    startRequest.requestId = pending.requestId;
    startRequest.rootPath = pending.request.rootPath;

    if (m_transportClient != nullptr && m_transportClient->SendStartScan(startRequest))
        return;

    FailActiveRequestForTransport(ERROR_BROKEN_PIPE, L"Failed to send StartScan over RPC transport.");
}

void RPCScanEngine::ResetRequestLifecycle()
{
    const std::uint64_t requestId = m_activeRequestId.load(std::memory_order_acquire);
    ASSERT(requestId != 0);
    ASSERT(GetActiveObserver() != nullptr);
    {
        std::scoped_lock lock(m_observerMutex);
        m_activeObserver = nullptr;
    }

    m_activeRequestId.store(0, std::memory_order_release);
    m_outstandingWorkItems.store(0, std::memory_order_release);
    m_requestInputClosed.store(true, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    ASSERT(m_activeRequestId.load(std::memory_order_acquire) == 0);
    ASSERT(GetActiveObserver() == nullptr);
    RpcClientLog(requestId, L"RequestReset",
        L"outstanding=0 inputClosed=1 cancelPending=0 running=0");
}

void RPCScanEngine::TryCloseRequestInput(const std::uint64_t requestId)
{
    if (!IsCurrentRequest(requestId))
        return;

    ASSERT(requestId != 0);
    ASSERT(m_outstandingWorkItems.load(std::memory_order_acquire) == 0);
    if (m_requestInputClosed.exchange(true, std::memory_order_acq_rel))
        return;

    RpcCloseRequestInput closeRequest{};
    closeRequest.requestId = requestId;
    RpcClientLog(requestId, L"CloseRequestInputSent",
        std::format(L"outstanding={}", m_outstandingWorkItems.load(std::memory_order_acquire)));
    if (m_transportClient != nullptr)
    {
        if (m_transportClient->SendCloseRequestInput(closeRequest))
            return;
    }

    FailActiveRequestForTransport(ERROR_BROKEN_PIPE, L"Failed to send CloseRequestInput over RPC transport.");
}

void RPCScanEngine::FailActiveRequestForTransport(const unsigned long errorCode, const std::wstring& message)
{
    const std::uint64_t requestId = m_activeRequestId.load(std::memory_order_acquire);
    if (requestId == 0)
        return;

    ASSERT(GetActiveObserver() != nullptr);
    if (m_terminalResolved.exchange(true, std::memory_order_acq_rel))
        return;

    IScanObserver* const observer = GetActiveObserver();
    ResetRequestLifecycle();

    if (observer != nullptr)
    {
        ScanError error{};
        error.requestId = requestId;
        error.path = L"RPC transport";
        error.message = message;
        error.errorCode = errorCode;
        observer->OnError(error);
        observer->OnCanceled(requestId, ScanTerminalReason::EngineInterrupted);
    }

    StartPendingRequestIfAny();
}
