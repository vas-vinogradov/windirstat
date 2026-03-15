#include "pch.h"
#include "ItemDiscoverySink.h"

#include "Item.h"
#include "BlockingQueue.h"
#include "DiscoveryBatch.h"
#include "ScanTask.h"

CItemDiscoverySink::CItemDiscoverySink(CDirStatDoc& doc) : m_doc(doc)
{
}

CItem* CItemDiscoverySink::FindItemByPath(const std::wstring& path) const
{
    CItem* rootItem = m_doc.GetRootItem();
    if (rootItem == nullptr)
        return nullptr;

    const std::wstring rootPath = rootItem->GetPath();

    if (path == rootPath)
        return rootItem;

    if (!path.starts_with(rootPath))
        return nullptr;

    std::wstring relative = path.substr(rootPath.size());

    while (!relative.empty() && relative.front() == wds::chrBackslash)
        relative.erase(relative.begin());

    if (relative.empty())
        return rootItem;

    const std::vector<std::wstring> components = SplitString(relative, wds::chrBackslash);

    CItem* current = rootItem;
    for (const auto& component : components)
    {
        if (current->IsLeaf())
            return nullptr;

        auto it = std::ranges::find_if(
            current->GetChildren(),
            [&](const CItem* child)
            {
                return child->GetNameView() == component;
            });

        if (it == current->GetChildren().end())
            return nullptr;

        current = *it;
    }

    return current;
}


std::vector<ScanTask> CItemDiscoverySink::Apply(const DiscoveryBatch& batch)
{
    std::vector<ScanTask> childTasks;

    CItem* item = FindItemByPath(batch.scannedPath);
    ASSERT(item != nullptr);
    if (item == nullptr)
    {
        return {};
    }

    for (const auto& dir : batch.directories)
    {
        item->UpwardAddFolders(1);
        CItem* newitem;
        newitem = item->AddDirectoryFromDiscovery(dir);
        if (newitem == nullptr)
        {
            continue;
        }
        
        TRACE("Discovered directory: %s (read jobs: %d)\n", dir.fullPath.c_str(), newitem->GetReadJobs());
        
        if (newitem->GetReadJobs() > 0)
        {
            childTasks.push_back(ScanTask{ newitem->GetPath() });
        }
    }

    for (const auto& file : batch.files)
    {
        item->UpwardAddFiles(1);

        CItem* newitem = item->AddFileFromDiscovery(file);

        // CFileDupeControl::Get()->ProcessDuplicate(newitem, &m_queue);
        CFileTopControl::Get()->ProcessTop(newitem);
    }

    CompleteTask(batch.scannedPath);

    return childTasks;
}

void CItemDiscoverySink::CompleteTask(const std::wstring& path)
{
    CItem* item = FindItemByPath(path);
    ASSERT(item != nullptr);
    if (item == nullptr)
    {
        return;
    }

    // Restore old loop completion semantics:
    // when one directory task is fully processed, subtract its read job.
    // If the subtree reaches zero outstanding jobs, UpwardSubtractReadJobs()
    // will trigger SetDone() naturally.
    item->UpwardSubtractReadJobs(1);
    item->UpwardDrivePacman();
}


