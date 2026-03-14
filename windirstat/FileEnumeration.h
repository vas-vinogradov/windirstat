#pragma once
#include <string>
#include <vector>

namespace FileEnumeration
{
    std::vector<std::wstring> EnumerateFileNames(
        const std::wstring& folder,
        const std::wstring& pattern);
}
