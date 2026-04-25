#include "pch.h"
#include "Engine/Rpc/RPCScanEngine.h"

#include "DirectoryProgressBatch.h"
#include "IScanObserver.h"
#include "ScanError.h"

namespace
{
void AppendRpcClientMetricsLog(const std::wstring_view line)
{
    const DWORD length = GetEnvironmentVariableW(L"WINDIRSTAT_RPC_CLIENT_LOG_FILE", nullptr, 0);
    if (length == 0)
        return;

    std::wstring path(length, L'\0');
    const DWORD actualLength = GetEnvironmentVariableW(L"WINDIRSTAT_RPC_CLIENT_LOG_FILE", path.data(), length);
    if (actualLength == 0 || actualLength >= length)
        return;

    path.resize(actualLength);
    std::ofstream stream(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream.is_open())
        return;

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return;

    std::string utf8(utf8Length, '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), utf8Length, nullptr, nullptr);
    stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    stream.write("\n", 1);
}

bool IsRpcVerboseLoggingEnabled()
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

bool IsExternalInputAuditEnabled()
{
    if (IsRpcVerboseLoggingEnabled())
        return true;

    std::array<wchar_t, 16> value{};
    const DWORD length = GetEnvironmentVariableW(L"WINDIRSTAT_RPC_EXTERNAL_INPUT_AUDIT", value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
        return false;

    std::wstring text(value.data(), length);
    _wcslwr_s(text.data(), text.size() + 1);
    return text == L"1" || text == L"true" || text == L"yes";
}

std::wstring RpcTimestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

void RpcClientLog(
    const std::uint64_t requestId,
    const std::wstring_view event,
    const std::wstring_view details = {},
    const bool verboseOnly = false)
{
    if (verboseOnly && !IsRpcVerboseLoggingEnabled())
        return;

    std::wstring line = std::format(L"[RPC][CLIENT] ts={} request={} event={}",
        RpcTimestamp(), requestId, event);
    if (!details.empty())
        line += std::format(L" {}", details);

    line += L"\n";
    OutputDebugStringW(line.c_str());
}

void RpcClientTrace(
    const std::uint64_t requestId,
    const std::wstring_view event,
    const std::wstring_view details = {},
    const bool verboseOnly = false)
{
    if (verboseOnly && !IsRpcVerboseLoggingEnabled())
        return;

    if (details.empty())
    {
        VTRACE(L"[RPC][CLIENT] request={} event={}", requestId, event);
        AppendRpcClientMetricsLog(std::format(L"[RPC][CLIENT] request={} event={}", requestId, event));
        return;
    }

    VTRACE(L"[RPC][CLIENT] request={} event={} {}", requestId, event, details);
    AppendRpcClientMetricsLog(std::format(L"[RPC][CLIENT] request={} event={} {}", requestId, event, details));
}

void RpcClientLifecycleLog(
    const std::uint64_t requestId,
    const std::wstring_view event,
    const std::wstring_view details = {},
    const bool verboseOnly = false)
{
    RpcClientLog(requestId, event, details, verboseOnly);
    RpcClientTrace(requestId, event, details, verboseOnly);
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
            RpcClientLifecycleLog(requestId, L"ReplacementQueued",
                std::format(L"replaces={} path=\"{}\"", activeRequestId, request.rootPath));
        }
        else
        {
            ActivateRequest(observer, requestId);
            if (request.expectMoreInputs)
                RecordExternalInputPath(request.rootPath);
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
        RpcClientLifecycleLog(requestToCancel, L"CancelRequested",
            std::format(L"reason={} replacement={}", static_cast<int>(cancelRequest.reason), requestId));

        if (m_transportClient != nullptr && m_transportClient->SendCancelScan(cancelRequest))
            return;

        FailActiveRequestForTransport(ERROR_BROKEN_PIPE, L"Failed to send CancelScan over RPC transport for replacement.");
        return;
    }

    RpcStartScanRequest startRequest{};
    startRequest.requestId = requestId;
    startRequest.rootPath = request.rootPath;
    startRequest.followMountPoints = !COptions::ExcludeVolumeMountPoints;
    startRequest.followSymbolicLinks = !COptions::ExcludeSymbolicLinksDirectory;
    startRequest.followJunctions = !COptions::ExcludeJunctions;
    RpcClientLifecycleLog(requestId, L"StartScanInvoked", std::format(L"path=\"{}\"", request.rootPath));

    if (m_transportClient != nullptr && m_transportClient->SendStartScan(startRequest))
    {
        RpcClientLifecycleLog(requestId, L"StartScanSent",
            std::format(L"path=\"{}\" expectMoreInputs={}", request.rootPath, request.expectMoreInputs ? 1 : 0));
        if (!request.expectMoreInputs)
            TryCloseRequestInput(requestId);
        return;
    }

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
    RecordExternalInputPath(request.rootPath);

    RpcEnqueueRequest enqueueRequest{};
    enqueueRequest.requestId = activeRequestId;
    enqueueRequest.rootPath = request.rootPath;
    RpcClientLifecycleLog(activeRequestId, L"EnqueueSent",
        std::format(L"path=\"{}\" outstanding={}", request.rootPath, m_outstandingWorkItems.load(std::memory_order_acquire)),
        true);

    if (m_transportClient != nullptr)
    {
        if (m_transportClient->SendEnqueue(enqueueRequest))
            return;
    }

    UndoExternalInputPath(request.rootPath);
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
    RpcClientLifecycleLog(requestId, L"CancelRequested",
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
    RpcClientLifecycleLog(event.requestId, L"DirectoryProgressReceived",
        std::format(L"path=\"{}\" finished={} outstanding={}",
            event.directoryPath, event.finished ? 1 : 0,
            m_outstandingWorkItems.load(std::memory_order_acquire)),
        true);

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

    if (!event.finished)
        return;

    const bool completedTrackedInput = CompleteExternalInputPath(event.directoryPath);
    if (IsExternalInputAuditEnabled())
    {
        RpcClientLifecycleLog(event.requestId, L"ExternalInputCompletionAudit",
            std::format(L"path=\"{}\" matchedTrackedInput={} state={}",
                event.directoryPath,
                completedTrackedInput ? 1 : 0,
                DescribeExternalInputState()));
    }

    if (completedTrackedInput)
    {
        const std::uint32_t remaining = m_outstandingWorkItems.load(std::memory_order_acquire);
        if (remaining == 0 && !m_cancelRequested.load(std::memory_order_acquire))
        {
            RpcClientLifecycleLog(event.requestId, L"CloseRequestInputAudit",
                std::format(L"action=SendClose path=\"{}\" state={}",
                    event.directoryPath, DescribeExternalInputState()));
            TryCloseRequestInput(event.requestId);
        }
        else
        {
            RpcClientLifecycleLog(event.requestId, L"CloseRequestInputAudit",
                std::format(L"action=Deferred path=\"{}\" remaining={} cancelPending={} inputClosed={} state={}",
                    event.directoryPath,
                    remaining,
                    m_cancelRequested.load(std::memory_order_acquire) ? 1 : 0,
                    m_requestInputClosed.load(std::memory_order_acquire) ? 1 : 0,
                    DescribeExternalInputState()));
        }
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
    RpcClientLifecycleLog(event.requestId, L"TerminalReceived", L"terminal=Completed");

    if (observer != nullptr)
        observer->OnCompleted(event.requestId);

    ResetRequestLifecycle();
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
    RpcClientLifecycleLog(event.requestId, L"TerminalReceived",
        std::format(L"terminal=Canceled reason={}", static_cast<int>(event.reason)));

    if (observer != nullptr)
        observer->OnCanceled(event.requestId, event.reason);

    ResetRequestLifecycle();
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
    RpcClientLifecycleLog(event.requestId, L"TerminalReceived",
        std::format(L"terminal=Failed path=\"{}\" errorCode={} message=\"{}\"",
            event.path, event.errorCode, event.message));

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

    ResetRequestLifecycle();
    StartPendingRequestIfAny();
}

void RPCScanEngine::OnTransportFailure(const unsigned long errorCode, const std::wstring& message)
{
    const std::uint64_t requestId = m_activeRequestId.load(std::memory_order_acquire);
    ASSERT(requestId == 0 || GetActiveObserver() != nullptr);
    RpcClientLifecycleLog(requestId, L"TransportFailure",
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
    m_outstandingWorkItems.store(0, std::memory_order_release);
    m_requestInputClosed.store(false, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);
    m_terminalResolved.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    RpcClientLifecycleLog(requestId, L"RequestActivated",
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
    RpcClientLifecycleLog(pending.requestId, L"PendingRequestPromotion",
        std::format(L"path=\"{}\"", pending.request.rootPath));
    ActivateRequest(*pending.observer, pending.requestId);
    if (pending.request.expectMoreInputs)
        RecordExternalInputPath(pending.request.rootPath);

    RpcStartScanRequest startRequest{};
    startRequest.requestId = pending.requestId;
    startRequest.rootPath = pending.request.rootPath;
    startRequest.followMountPoints = !COptions::ExcludeVolumeMountPoints;
    startRequest.followSymbolicLinks = !COptions::ExcludeSymbolicLinksDirectory;
    startRequest.followJunctions = !COptions::ExcludeJunctions;

    if (m_transportClient != nullptr && m_transportClient->SendStartScan(startRequest))
    {
        RpcClientLifecycleLog(pending.requestId, L"StartScanSent",
            std::format(L"path=\"{}\" expectMoreInputs={}", pending.request.rootPath, pending.request.expectMoreInputs ? 1 : 0));
        if (!pending.request.expectMoreInputs)
            TryCloseRequestInput(pending.requestId);
        return;
    }

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
    ResetExternalInputTracking();
    m_requestInputClosed.store(true, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    ASSERT(m_activeRequestId.load(std::memory_order_acquire) == 0);
    ASSERT(GetActiveObserver() == nullptr);
    RpcClientLifecycleLog(requestId, L"RequestReset",
        L"outstanding=0 inputClosed=1 cancelPending=0 running=0",
        true);
}

void RPCScanEngine::RecordExternalInputPath(const std::wstring& path)
{
    ASSERT(!path.empty());
    std::scoped_lock lock(m_inputTrackingMutex);
    ++m_pendingInputPaths[path];
    const std::uint32_t outstanding = m_outstandingWorkItems.fetch_add(1, std::memory_order_acq_rel) + 1;
    RpcClientLifecycleLog(m_activeRequestId.load(std::memory_order_acquire), L"ExternalInputRecorded",
        std::format(L"path=\"{}\" pendingForPath={} outstanding={} trackedPaths={}",
            path,
            m_pendingInputPaths[path],
            outstanding,
            m_pendingInputPaths.size()));
}

void RPCScanEngine::UndoExternalInputPath(const std::wstring& path)
{
    ASSERT(!path.empty());
    std::wstring state;
    std::scoped_lock lock(m_inputTrackingMutex);
    const auto it = m_pendingInputPaths.find(path);
    ASSERT(it != m_pendingInputPaths.end());
    if (it == m_pendingInputPaths.end())
        return;

    ASSERT(it->second > 0);
    if (--it->second == 0)
        m_pendingInputPaths.erase(it);

    const std::uint32_t outstanding = m_outstandingWorkItems.load(std::memory_order_acquire);
    ASSERT(outstanding > 0);
    if (outstanding > 0)
        m_outstandingWorkItems.fetch_sub(1, std::memory_order_acq_rel);
    state = DescribeExternalInputStateLocked();

    RpcClientLifecycleLog(m_activeRequestId.load(std::memory_order_acquire), L"ExternalInputUndo",
        std::format(L"path=\"{}\" state={}", path, state),
        true);
}

bool RPCScanEngine::CompleteExternalInputPath(const std::wstring& path)
{
    ASSERT(!path.empty());
    std::wstring state;
    std::scoped_lock lock(m_inputTrackingMutex);
    const auto it = m_pendingInputPaths.find(path);
    if (it == m_pendingInputPaths.end())
    {
        RpcClientLifecycleLog(m_activeRequestId.load(std::memory_order_acquire), L"ExternalInputNoMatch",
            std::format(L"path=\"{}\" outstanding={} trackedPaths={}",
                path,
                m_outstandingWorkItems.load(std::memory_order_acquire),
                m_pendingInputPaths.size()),
            true);
        return false;
    }

    ASSERT(it->second > 0);
    if (--it->second == 0)
        m_pendingInputPaths.erase(it);

    const std::uint32_t outstanding = m_outstandingWorkItems.load(std::memory_order_acquire);
    ASSERT(outstanding > 0);
    if (outstanding > 0)
        m_outstandingWorkItems.fetch_sub(1, std::memory_order_acq_rel);
    state = DescribeExternalInputStateLocked();

    RpcClientLifecycleLog(m_activeRequestId.load(std::memory_order_acquire), L"ExternalInputCompleted",
        std::format(L"path=\"{}\" state={}", path, state),
        false);

    return true;
}

void RPCScanEngine::ResetExternalInputTracking()
{
    std::scoped_lock lock(m_inputTrackingMutex);
    m_pendingInputPaths.clear();
    m_outstandingWorkItems.store(0, std::memory_order_release);
}

std::wstring RPCScanEngine::DescribeExternalInputState() const
{
    std::scoped_lock lock(m_inputTrackingMutex);
    return DescribeExternalInputStateLocked();
}

std::wstring RPCScanEngine::DescribeExternalInputStateLocked() const
{
    std::wstring samplePaths;
    std::size_t emitted = 0;
    for (const auto& [path, count] : m_pendingInputPaths)
    {
        if (emitted > 0)
            samplePaths += L";";

        samplePaths += std::format(L"\"{}\"x{}", path, count);
        ++emitted;
        if (emitted == 3)
            break;
    }

    return std::format(L"outstanding={} trackedPaths={} samples=[{}]",
        m_outstandingWorkItems.load(std::memory_order_acquire),
        m_pendingInputPaths.size(),
        samplePaths);
}

void RPCScanEngine::TryCloseRequestInput(const std::uint64_t requestId)
{
    if (!IsCurrentRequest(requestId))
        return;

    ASSERT(requestId != 0);
    ASSERT(m_outstandingWorkItems.load(std::memory_order_acquire) == 0);
    if (m_requestInputClosed.exchange(true, std::memory_order_acq_rel))
    {
        RpcClientLifecycleLog(requestId, L"CloseRequestInputAudit",
            std::format(L"action=SkipAlreadyClosed state={}", DescribeExternalInputState()),
            false);
        return;
    }

    RpcCloseRequestInput closeRequest{};
    closeRequest.requestId = requestId;
    RpcClientLifecycleLog(requestId, L"CloseRequestInputSent",
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

    ResetRequestLifecycle();
    StartPendingRequestIfAny();
}
