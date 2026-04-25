#pragma once

#include "DiscoveredDirectory.h"
#include "DiscoveredFile.h"
#include "RemoteStub/DirectoryWorkItemProcessor.h"

#include <string>
#include <variant>
#include <vector>

struct DirectoryWorkItemPolicyInput
{
    DirectoryWorkItemResult workItemResult{};
};

struct DirectoryProgressPayload
{
    std::wstring directoryPath;
    std::vector<DiscoveredFile> files;
    std::vector<DiscoveredDirectory> directories;
    bool finished = true;
};

struct DirectoryFailurePayload
{
    std::wstring path;
    std::wstring message;
    unsigned long errorCode = 0;
};

struct DirectoryWorkItemPolicyResult
{
    std::variant<DirectoryProgressPayload, DirectoryFailurePayload> payload;
};

class DirectoryWorkItemHostPolicy
{
public:
    DirectoryWorkItemPolicyResult Apply(DirectoryWorkItemPolicyInput input) const;
};
