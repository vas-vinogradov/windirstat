#pragma once

#include "UiScanDtos.h"

class CDirStatDoc;
class CItem;

class CItemScanResultAdapter
{
public:
    explicit CItemScanResultAdapter(CDirStatDoc& doc);

    void ApplyDirectoryResult(const DirectoryResultDto& result) const;

private:
    CItem* FindItemByPath(const std::wstring& path) const;
    static std::wstring BuildChildKey(const CItem* item);
    static std::wstring BuildChildKey(const UiScanEntryDto& entry);
    void RemoveProjectedChild(CItem* parent, CItem* child) const;
    void AddDirectory(CItem* parent, const DirectoryResultDto& result, const UiScanEntryDto& entry) const;
    void AddFile(CItem* parent, const UiScanEntryDto& entry) const;

    CDirStatDoc& m_doc;
};
