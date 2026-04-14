#include "pch.h"
#include "RemoteStub/StubDirectoryDiscoveryEngine.h"

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

bool PathContainsInsensitive(const std::wstring& path, const std::wstring& token)
{
    std::wstring normalizedPath = path;
    std::wstring normalizedToken = token;
    _wcslwr_s(normalizedPath.data(), normalizedPath.size() + 1);
    _wcslwr_s(normalizedToken.data(), normalizedToken.size() + 1);
    return normalizedPath.find(normalizedToken) != std::wstring::npos;
}
}

DiscoveryBatch StubDirectoryDiscoveryEngine::Discover(const DirectoryDiscoveryRequest& request)
{
    DiscoveryBatch batch{};
    batch.scannedPath = request.path;

    if (PathContainsInsensitive(request.path, L"stub-empty"))
        return batch;

    DiscoveredFile file{};
    file.name = L"stub-file.txt";
    file.fullPath = JoinChildPath(request.path, file.name);
    file.sizePhysical = 128;
    file.sizeLogical = 128;
    file.index = 1;
    file.attributes = FILE_ATTRIBUTE_NORMAL;
    batch.files.push_back(std::move(file));

    if (PathContainsInsensitive(request.path, L"stub-dir"))
    {
        DiscoveredDirectory directory{};
        directory.name = L"stub-child";
        directory.fullPath = JoinChildPath(request.path, directory.name);
        directory.index = 2;
        directory.attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
        directory.reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
        directory.isProtectedReparsePoint = true;
        batch.directories.push_back(std::move(directory));
    }

    return batch;
}
