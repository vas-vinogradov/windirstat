#include "pch.h"
#include "CItemScanResultAdapter.h"

#include "DirStatDoc.h"
#include "FileDupeControl.h"
#include "FileSearchControl.h"
#include "FileTopControl.h"
#include "Item.h"
#include "UiScanProjectionPlan.h"

CItemScanResultAdapter::CItemScanResultAdapter(CDirStatDoc& doc)
    : m_doc(doc)
{
}

CItem* CItemScanResultAdapter::FindItemByPath(const std::wstring& path) const
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

std::wstring CItemScanResultAdapter::BuildChildKey(const CItem* item)
{
    std::wstring key = item->IsTypeOrFlag(IT_FILE) ? L"F:" : L"D:";
    std::wstring name(item->GetNameView());
    _wcslwr_s(name.data(), name.size() + 1);
    key += name;
    return key;
}

std::wstring CItemScanResultAdapter::BuildChildKey(const UiScanEntryDto& entry)
{
    return BuildUiScanChildKey(entry.type, entry.name);
}

void CItemScanResultAdapter::RemoveProjectedChild(CItem* parent, CItem* child) const
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

void CItemScanResultAdapter::AddDirectory(CItem* parent, const DirectoryResultDto& result, const UiScanEntryDto& entry) const
{
    parent->UpwardAddFolders(1);
    const bool follow = !entry.isProtectedReparsePoint &&
        CDirStatApp::Get()->IsFollowingAllowed(entry.reparseTag);
    CItem* newItem = parent->AddDirectoryFromUiScanEntry(entry, follow);
    const IScanEngine* const engine = m_doc.GetScanEngine();
    if (follow && result.requestId == m_doc.GetActiveScanRequestId() && engine != nullptr && !engine->OwnsTraversal())
    {
        ScanRequest request{};
        request.requestId = result.requestId;
        request.rootPath = newItem->GetPath();
        m_doc.StartScan(request);
    }
}

void CItemScanResultAdapter::AddFile(CItem* parent, const UiScanEntryDto& entry) const
{
    parent->UpwardAddFiles(1);
    CItem* newItem = parent->AddFileFromUiScanEntry(entry);
    CFileTopControl::Get()->ProcessTop(newItem);
}

void CItemScanResultAdapter::ApplyDirectoryResult(const DirectoryResultDto& result) const
{
    AssertValidDirectoryResultDto(result);

    CItem* item = FindItemByPath(result.path);
    ASSERT(item != nullptr);
    if (item == nullptr)
        return;

    std::unordered_map<std::wstring, CItem*> existingChildren;
    std::vector<std::wstring> existingChildKeys;
    for (CItem* child : item->GetChildren())
    {
        const std::wstring key = BuildChildKey(child);
        existingChildren[key] = child;
        existingChildKeys.push_back(key);
    }

    const UiScanProjectionPlan plan = BuildUiScanProjectionPlan(result, existingChildKeys);
    for (const std::wstring& key : plan.childKeysToRemove)
    {
        if (const auto existing = existingChildren.find(key);
            existing != existingChildren.end())
        {
            RemoveProjectedChild(item, existing->second);
            existingChildren.erase(existing);
        }
    }

    for (const UiScanEntryDto* entry : plan.directoriesToProject)
    {
        ASSERT(entry != nullptr);
        AddDirectory(item, result, *entry);
    }

    for (const UiScanEntryDto* entry : plan.filesToProject)
    {
        ASSERT(entry != nullptr);
        AddFile(item, *entry);
    }

    if (!result.finished)
        return;

    item->UpwardSubtractReadJobs(1);
    item->UpwardDrivePacman();
}
