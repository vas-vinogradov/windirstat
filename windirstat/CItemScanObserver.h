#pragma once

#include "IScanObserver.h"

class CDirStatDoc;

class CItemScanObserver final : public IScanObserver
{
public:
    explicit CItemScanObserver(CDirStatDoc& doc);

    void OnDirectoryProgress(DirectoryResultDto result) override;
    void OnCompleted(std::uint64_t requestId) override;
    void OnCanceled(std::uint64_t requestId, ScanTerminalReason reason) override;
    void OnError(const ScanError& error) override;

private:
    bool IsActiveRequest(std::uint64_t requestId) const;

    CDirStatDoc& m_doc;
};
