#pragma once

#include "UiScanDtos.h"

#include <string>
#include <vector>

std::wstring BuildUiScanChildKey(UiScanEntryType type, std::wstring_view name);

inline bool IsDirectoryResultForActiveRequest(const DirectoryResultDto& result, const std::uint64_t activeRequestId)
{
    return result.requestId == activeRequestId;
}

struct UiScanProjectionPlan
{
    std::vector<std::wstring> childKeysToRemove;
    std::vector<const UiScanEntryDto*> directoriesToProject;
    std::vector<const UiScanEntryDto*> filesToProject;
};

// Intent: pure DTO projection planning for UI adapters. This keeps reconciliation
// rules testable without depending on CItem, MFC, RPC, or scan-engine types.
UiScanProjectionPlan BuildUiScanProjectionPlan(
    const DirectoryResultDto& result,
    const std::vector<std::wstring>& existingChildKeys);

void AssertValidDirectoryResultDto(const DirectoryResultDto& result);
