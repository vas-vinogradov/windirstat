#include "pch.h"
#include "ItemDiscoverySink.h"

#include "Item.h"              // or CItem.h / whatever your actual file is
#include "BlockingQueue.h"

struct DiscoveryBatch;

CItemDiscoverySink::CItemDiscoverySink(CItem& parent, BlockingQueue<CItem*>& queue)
    : m_parent(parent)
    , m_queue(queue)
{
}

void CItemDiscoverySink::Apply(const DiscoveryBatch& batch)
{
    for (const auto& dir : batch.directories)
    {
        CItem* child = m_parent.AddDirectoryFromDiscovery(dir);
        if (child != nullptr)
        {
            m_queue.Push(child);
        }
    }

    for (const auto& file : batch.files)
    {
        m_parent.AddFileFromDiscovery(file);
    }
}
