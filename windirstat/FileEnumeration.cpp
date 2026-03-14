#include "pch.h"
#include "FileEnumeration.h"

#include <windows.h>

namespace FileEnumeration
{
    std::vector<std::wstring> EnumerateFileNames(
        const std::wstring& folder,
        const std::wstring& pattern)
    {
        std::vector<std::wstring> result;

        std::wstring search = folder;
        if (!search.empty() && search.back() != L'\\' && search.back() != L'/')
            search += L'\\';
        search += pattern;

        WIN32_FIND_DATAW data{};
        HANDLE h = FindFirstFileW(search.c_str(), &data);
        if (h == INVALID_HANDLE_VALUE)
            return result;

        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                result.emplace_back(data.cFileName);
            }
        } while (FindNextFileW(h, &data));

        FindClose(h);
        return result;
    }
}
