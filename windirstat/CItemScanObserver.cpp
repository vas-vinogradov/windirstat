#include "pch.h"
#include "CItemScanObserver.h"

#include "CItemScanResultAdapter.h"
#include "DirStatDoc.h"

CItemScanObserver::CItemScanObserver(CDirStatDoc& doc)
    : m_doc(doc)
{
}

bool CItemScanObserver::IsActiveRequest(const std::uint64_t requestId) const
{
    return requestId == m_doc.GetActiveScanRequestId();
}

void CItemScanObserver::OnDirectoryProgress(DirectoryResultDto result)
{
    if (!IsActiveRequest(result.requestId))
        return;

    // Intent: keep observer responsibilities narrow. Scan engines emit neutral
    // DTOs; this transitional adapter is the only CItem/MFC projection point.
    CItemScanResultAdapter(m_doc).ApplyDirectoryResult(result);
}

void CItemScanObserver::OnCompleted(const std::uint64_t requestId)
{
    if (!IsActiveRequest(requestId))
        return;

    m_doc.FinalizeScan(requestId, false);
}

void CItemScanObserver::OnCanceled(const std::uint64_t requestId, const ScanTerminalReason reason)
{
    if (!IsActiveRequest(requestId))
        return;

    m_doc.FinalizeScan(requestId, true, reason);
}

void CItemScanObserver::OnError(const ScanError& error)
{
    if (!IsActiveRequest(error.requestId))
        return;

    VTRACE(L"Legacy scan error at '{}': {} ({})", error.path, error.message, error.errorCode);
}
