#pragma once

#include "IDiscoverySink.h"

struct ScanTask;
class CItem;
class CDirStatDoc;
template<typename T>
class BlockingQueue;

class CItemDiscoverySink : public IDiscoverySink
{
public:
    explicit CItemDiscoverySink(CDirStatDoc& doc);

    std::vector<ScanTask> Apply(const DiscoveryBatch& batch) override;
private:
    CItem* FindItemByPath(const std::wstring& path) const;
    
    CDirStatDoc& m_doc;
};
