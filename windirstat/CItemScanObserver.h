#pragma once

#include "IScanObserver.h"

class CDirStatDoc;
class CItem;

class CItemScanObserver final : public IScanObserver
{
public:
    explicit CItemScanObserver(CDirStatDoc& doc);

    void OnDirectoryProgress(DirectoryProgressBatch batch) override;
    void OnCompleted(std::uint64_t requestId) override;
    void OnCanceled(std::uint64_t requestId, ScanTerminalReason reason) override;
    void OnError(const ScanError& error) override;

private:
    bool IsActiveRequest(std::uint64_t requestId) const;
    CItem* FindItemByPath(const std::wstring& path) const;
    static std::wstring BuildChildKey(const CItem* item);
    static std::wstring BuildChildKey(const DiscoveredDirectory& dir);
    static std::wstring BuildChildKey(const DiscoveredFile& file);
    void RemoveProjectedChild(CItem* parent, CItem* child) const;

    CDirStatDoc& m_doc;
};
