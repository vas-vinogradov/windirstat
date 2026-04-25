#include "pch.h"
#include "UiScanProjectionPlan.h"

std::wstring BuildUiScanChildKey(const UiScanEntryType type, const std::wstring_view name)
{
    std::wstring key = type == UiScanEntryType::File ? L"F:" : L"D:";
    key += name;
    _wcslwr_s(key.data(), key.size() + 1);
    return key;
}

void AssertValidDirectoryResultDto(const DirectoryResultDto& result)
{
    ASSERT(!result.path.empty());

#ifdef _DEBUG
    std::unordered_set<std::wstring> entryKeys;
    for (const UiScanEntryDto& entry : result.entries)
    {
        ASSERT(!entry.name.empty());
        ASSERT(entry.fullPath.empty() || entry.fullPath.starts_with(result.path));

        const std::wstring key = BuildUiScanChildKey(entry.type, entry.name);
        ASSERT(!entryKeys.contains(key));
        entryKeys.insert(key);
    }
#else
    (void)result;
#endif
}

UiScanProjectionPlan BuildUiScanProjectionPlan(
    const DirectoryResultDto& result,
    const std::vector<std::wstring>& existingChildKeys)
{
    AssertValidDirectoryResultDto(result);

    UiScanProjectionPlan plan{};
    std::unordered_set<std::wstring> remainingExisting(existingChildKeys.begin(), existingChildKeys.end());
    std::unordered_set<std::wstring> removalKeys;

    auto scheduleReplacementRemoval = [&](const UiScanEntryDto& entry)
    {
        const std::wstring key = BuildUiScanChildKey(entry.type, entry.name);
        if (remainingExisting.erase(key) != 0 && removalKeys.insert(key).second)
            plan.childKeysToRemove.push_back(key);
    };

    for (const UiScanEntryDto& entry : result.entries)
    {
        if (entry.type != UiScanEntryType::Directory)
            continue;

        scheduleReplacementRemoval(entry);
        plan.directoriesToProject.push_back(&entry);
    }

    for (const UiScanEntryDto& entry : result.entries)
    {
        if (entry.type != UiScanEntryType::File)
            continue;

        scheduleReplacementRemoval(entry);
        plan.filesToProject.push_back(&entry);
    }

    if (result.finished)
    {
        for (const std::wstring& key : remainingExisting)
        {
            if (removalKeys.insert(key).second)
                plan.childKeysToRemove.push_back(key);
        }
    }

    return plan;
}
