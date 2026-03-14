#pragma once

#include "IDiscoverySink.h"

class CItem;
template<typename T>
class BlockingQueue;

class CItemDiscoverySink : public IDiscoverySink
{
public:
    CItemDiscoverySink(CItem& parent, BlockingQueue<CItem*>& queue);

    void Apply(const DiscoveryBatch& batch) override;

private:
    CItem& m_parent;
    BlockingQueue<CItem*>& m_queue;
};
