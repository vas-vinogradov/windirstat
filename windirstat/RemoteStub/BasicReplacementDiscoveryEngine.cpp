#include "pch.h"
#include "RemoteStub/BasicReplacementDiscoveryEngine.h"

#include "Finder.h"

#include <system_error>

namespace
{
std::wstring JoinChildPath(const std::wstring& parent, const std::wstring& childName)
{
    if (parent.empty())
        return childName;

    if (parent.back() == L'\\')
        return parent + childName;

    return parent + L"\\" + childName;
}

std::wstring MakeSearchPattern(const std::wstring& directoryPath)
{
    std::wstring searchPath = Finder::MakeLongPathCompatible(directoryPath);
    if (!searchPath.empty() && searchPath.back() != L'\\')
        searchPath += L'\\';

    searchPath += L"*";
    return searchPath;
}

bool IsDotEntry(const wchar_t* fileName)
{
    return wcscmp(fileName, L".") == 0 || wcscmp(fileName, L"..") == 0;
}

void ThrowLastError(const char* operation)
{
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

ULONGLONG CombineHighLow(const DWORD high, const DWORD low)
{
    ULARGE_INTEGER value{};
    value.HighPart = high;
    value.LowPart = low;
    return value.QuadPart;
}

ULONGLONG TryGetFileIndex(const std::wstring& fullPath, const DWORD attributes)
{
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        flags |= FILE_FLAG_BACKUP_SEMANTICS;

    const std::wstring longPath = Finder::MakeLongPathCompatible(fullPath);
    const HANDLE handle = CreateFileW(longPath.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool success = GetFileInformationByHandle(handle, &info) != FALSE;
    CloseHandle(handle);

    return success ? CombineHighLow(info.nFileIndexHigh, info.nFileIndexLow) : 0;
}

ULONGLONG GetLogicalSize(const WIN32_FIND_DATAW& findData)
{
    return CombineHighLow(findData.nFileSizeHigh, findData.nFileSizeLow);
}

ULONGLONG TryGetPhysicalSize(const std::wstring& fullPath, const ULONGLONG fallbackLogicalSize)
{
    const std::wstring longPath = Finder::MakeLongPathCompatible(fullPath);
    DWORD highPart = 0;
    const DWORD lowPart = GetCompressedFileSizeW(longPath.c_str(), &highPart);
    if (lowPart == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        return fallbackLogicalSize;

    return CombineHighLow(highPart, lowPart);
}

DWORD GetReparseTag(const WIN32_FIND_DATAW& findData)
{
    return (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        ? findData.dwReserved0
        : 0;
}

bool IsProtectedReparsePoint(const DWORD attributes)
{
    constexpr DWORD protectedAttributes =
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT;
    return (attributes & protectedAttributes) == protectedAttributes;
}

bool IsOffVolumeReparsePoint(const DWORD reparseTag)
{
    return reparseTag == IO_REPARSE_TAG_MOUNT_POINT ||
        reparseTag == IO_REPARSE_TAG_SYMLINK;
}
}

DiscoveryBatch BasicReplacementDiscoveryEngine::Discover(const DirectoryDiscoveryRequest& request)
{
    DiscoveryBatch batch{};
    batch.scannedPath = request.path;

    const std::wstring searchPattern = MakeSearchPattern(request.path);
    WIN32_FIND_DATAW findData{};
    const HANDLE findHandle = FindFirstFileW(searchPattern.c_str(), &findData);
    if (findHandle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
            return batch;

        SetLastError(error);
        ThrowLastError("BasicReplacementDiscoveryEngine::Discover");
    }

    bool done = false;
    while (!done)
    {
        if (!IsDotEntry(findData.cFileName))
        {
            const std::wstring childName = findData.cFileName;
            const std::wstring fullPath = JoinChildPath(request.path, childName);
            const DWORD attributes = findData.dwFileAttributes;
            const DWORD reparseTag = GetReparseTag(findData);

            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                DiscoveredDirectory directory{};
                directory.name = childName;
                directory.fullPath = fullPath;
                directory.index = TryGetFileIndex(fullPath, attributes);
                directory.lastChange = findData.ftLastWriteTime;
                directory.attributes = attributes;
                directory.reparseTag = reparseTag;
                directory.isReserved = false;
                directory.isOffVolume = IsOffVolumeReparsePoint(reparseTag);
                directory.isProtectedReparsePoint = IsProtectedReparsePoint(attributes);
                batch.directories.push_back(std::move(directory));
            }
            else
            {
                DiscoveredFile file{};
                file.name = childName;
                file.fullPath = fullPath;
                file.index = TryGetFileIndex(fullPath, attributes);
                file.lastChange = findData.ftLastWriteTime;
                file.attributes = attributes;
                file.reparseTag = reparseTag;
                file.isReserved = false;
                file.sizeLogical = GetLogicalSize(findData);
                file.sizePhysical = TryGetPhysicalSize(fullPath, file.sizeLogical);
                batch.files.push_back(std::move(file));
            }
        }

        if (FindNextFileW(findHandle, &findData) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES)
            {
                done = true;
            }
            else
            {
                FindClose(findHandle);
                SetLastError(error);
                ThrowLastError("BasicReplacementDiscoveryEngine::Discover");
            }
        }
    }

    FindClose(findHandle);
    return batch;
}
