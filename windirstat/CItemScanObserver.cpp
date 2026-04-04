#include "pch.h"
#include "CItemScanObserver.h"

#include "DirStatDoc.h"
#include "FileDupeControl.h"
#include "FileSearchControl.h"
#include "FileTopControl.h"
#include "Item.h"

CItemScanObserver::CItemScanObserver(CDirStatDoc& doc)
    : m_doc(doc)
{
}

bool CItemScanObserver::IsActiveRequest(const std::uint64_t requestId) const
{
    return requestId == m_doc.GetActiveScanRequestId();
}

CItem* CItemScanObserver::FindItemByPath(const std::wstring& path) const
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

        const auto children = current->GetChildren();
        const auto it = std::ranges::find_if(children, [&](const CItem* child)
        {
            return child->GetNameView() == component;
        });

        if (it == children.end())
            return nullptr;

        current = *it;
    }

    return current;
}

std::wstring CItemScanObserver::BuildChildKey(const CItem* item)
{
    std::wstring key = item->IsTypeOrFlag(IT_FILE) ? L"F:" : L"D:";
    std::wstring name(item->GetNameView());
    _wcslwr_s(name.data(), name.size() + 1);
    key += name;
    return key;
}

std::wstring CItemScanObserver::BuildChildKey(const DiscoveredDirectory& dir)
{
    std::wstring key = L"D:" + dir.name;
    _wcslwr_s(key.data(), key.size() + 1);
    return key;
}

std::wstring CItemScanObserver::BuildChildKey(const DiscoveredFile& file)
{
    std::wstring key = L"F:" + file.name;
    _wcslwr_s(key.data(), key.size() + 1);
    return key;
}

void CItemScanObserver::RemoveProjectedChild(CItem* parent, CItem* child) const
{
    CFileDupeControl::Get()->RemoveItem(child);
    CFileTopControl::Get()->RemoveItem(child);
    CFileSearchControl::Get()->RemoveItem(child);

    child->ExtensionDataProcessChildren(true);
    parent->UpwardRecalcLastChange();
    parent->UpwardSubtractSizePhysical(child->GetSizePhysical());
    parent->UpwardSubtractSizeLogical(child->GetSizeLogical());
    parent->UpwardSubtractFiles(child->IsTypeOrFlag(IT_FILE) ? 1 : child->GetFilesCount());
    parent->UpwardSubtractFolders(child->IsTypeOrFlag(IT_DIRECTORY, IT_DRIVE) ? child->GetFoldersCount() + 1 : 0);
    parent->RemoveChild(child);
}

void CItemScanObserver::OnDirectoryProgress(DirectoryProgressBatch batch)
{
    if (!IsActiveRequest(batch.requestId))
        return;

    CItem* item = FindItemByPath(batch.directoryPath);
    ASSERT(item != nullptr);
    if (item == nullptr)
        return;

    std::unordered_map<std::wstring, CItem*> existingChildren;
    for (CItem* child : item->GetChildren())
    {
        existingChildren[BuildChildKey(child)] = child;
    }

    for (const auto& directory : batch.directories)
    {
        if (const auto existing = existingChildren.find(BuildChildKey(directory));
            existing != existingChildren.end())
        {
            RemoveProjectedChild(item, existing->second);
            existingChildren.erase(existing);
        }

        item->UpwardAddFolders(1);
        const bool follow = !directory.isProtectedReparsePoint &&
            CDirStatApp::Get()->IsFollowingAllowed(directory.reparseTag);
        CItem* newitem = item->AddDirectoryFromDiscovery(directory, follow);
        if (follow && IsActiveRequest(batch.requestId))
        {
            ScanRequest request{};
            request.requestId = batch.requestId;
            request.rootPath = newitem->GetPath();
            m_doc.StartScan(request);
        }
    }

    for (const auto& file : batch.files)
    {
        if (const auto existing = existingChildren.find(BuildChildKey(file));
            existing != existingChildren.end())
        {
            RemoveProjectedChild(item, existing->second);
            existingChildren.erase(existing);
        }

        item->UpwardAddFiles(1);
        CItem* newitem = item->AddFileFromDiscovery(file);
        CFileTopControl::Get()->ProcessTop(newitem);
    }

    if (batch.finished)
    {
        // finished=true is the reconciliation boundary: this batch is the
        // authoritative immediate child snapshot for batch.directoryPath, so
        // previously known children omitted here may be removed.
        for (const auto& child : existingChildren | std::views::values)
        {
            RemoveProjectedChild(item, child);
        }

        item->UpwardSubtractReadJobs(1);
        item->UpwardDrivePacman();
    }
}

void CItemScanObserver::OnCompleted(const std::uint64_t requestId)
{
    if (!IsActiveRequest(requestId))
        return;

    m_doc.FinalizeScan(requestId, false);
}

void CItemScanObserver::OnCanceled(const std::uint64_t requestId, const ScanTerminalReason reason)
{
    if (!IsActiveRequest(requestId))
        return;

    m_doc.FinalizeScan(requestId, true, reason);
}

void CItemScanObserver::OnError(const ScanError& error)
{
    if (!IsActiveRequest(error.requestId))
        return;

    VTRACE(L"Legacy scan error at '{}': {} ({})", error.path, error.message, error.errorCode);
}
