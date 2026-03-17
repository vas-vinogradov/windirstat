#pragma once

#include <string>
#include <vector>
#include "IDiscoverySink.h"
#include "Item.h"
#include "DirStatDoc.h"

class CItemDiscoverySink : public IDiscoverySink
{
public:
    explicit CItemDiscoverySink(CDirStatDoc& doc);

    std::vector<ScanTask> Apply(const DiscoveryBatch& batch) override;
    void CompleteTask(const std::wstring& path) override;

private:
    CDirStatDoc& m_doc;
    CItem* FindItemByPath(const std::wstring& path) const;
};
