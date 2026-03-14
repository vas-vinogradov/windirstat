#include "pch.h"
#include "ItemDiscoverySink.h"

#include "Item.h"
#include "BlockingQueue.h"
#include "DiscoveryBatch.h"

CItemDiscoverySink::CItemDiscoverySink(BlockingQueue<CItem*>& queue)
    : m_queue(queue)
{
}

void CItemDiscoverySink::Apply(CItem& item, const DiscoveryBatch& batch)
{
    for (const auto& dir : batch.directories) {
        item.UpwardAddFolders(1);

        if (CItem* newitem = item.AddDirectoryFromDiscovery(dir); newitem->GetReadJobs() > 0) {
            m_queue.Push(newitem);
        }
    }

    for (const auto& file : batch.files) {
        item.UpwardAddFiles(1);

        CItem* newitem = item.AddFileFromDiscovery(file);

        CFileDupeControl::Get()->ProcessDuplicate(newitem, &m_queue);
        CFileTopControl::Get()->ProcessTop(newitem);

        m_queue.WaitIfSuspended();
    }

    item.UpwardDrivePacman();
}
