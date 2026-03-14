#pragma once

#include "IDiscoverySink.h"

class CItem;
template<typename T>
class BlockingQueue;

class CItemDiscoverySink : public IDiscoverySink
{
public:
    explicit CItemDiscoverySink(BlockingQueue<CItem*>& queue);

    void Apply(CItem& item, const DiscoveryBatch& batch) override;

private:
    BlockingQueue<CItem*>& m_queue;
};
