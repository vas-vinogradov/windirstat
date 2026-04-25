#include "pch.h"
#include "UiScanDtoMapper.h"

#include "DiscoveredDirectory.h"
#include "DiscoveredFile.h"
#include "Engine/Rpc/RpcMessages.h"

namespace
{
std::uint64_t ToFileTimeTicks(const FILETIME& value)
{
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

UiScanEntryDto ToDto(const DiscoveredFile& file)
{
    UiScanEntryDto dto{};
    dto.type = UiScanEntryType::File;
    dto.name = file.name;
    dto.fullPath = file.fullPath;
    dto.sizeLogical = file.sizeLogical;
    dto.sizePhysical = file.sizePhysical;
    dto.fileIndex = file.index;
    dto.lastChangeFileTime = ToFileTimeTicks(file.lastChange);
    dto.attributes = file.attributes;
    dto.reparseTag = file.reparseTag;
    dto.isReserved = file.isReserved;
    return dto;
}

UiScanEntryDto ToDto(const DiscoveredDirectory& directory)
{
    UiScanEntryDto dto{};
    dto.type = UiScanEntryType::Directory;
    dto.name = directory.name;
    dto.fullPath = directory.fullPath;
    dto.fileIndex = directory.index;
    dto.lastChangeFileTime = ToFileTimeTicks(directory.lastChange);
    dto.attributes = directory.attributes;
    dto.reparseTag = directory.reparseTag;
    dto.isReserved = directory.isReserved;
    dto.isOffVolume = directory.isOffVolume;
    dto.isProtectedReparsePoint = directory.isProtectedReparsePoint;
    return dto;
}
}

DirectoryResultDto BuildDirectoryResultDto(
    const std::uint64_t requestId,
    std::wstring directoryPath,
    std::vector<DiscoveredFile> files,
    std::vector<DiscoveredDirectory> directories,
    const bool finished)
{
    DirectoryResultDto dto{};
    dto.requestId = requestId;
    dto.path = std::move(directoryPath);
    dto.finished = finished;
    dto.entries.reserve(directories.size() + files.size());

    // Contract: preserve the current projection order: directories first, then
    // files. Future clients should not infer traversal ownership from this.
    for (const DiscoveredDirectory& directory : directories)
        dto.entries.push_back(ToDto(directory));
    for (const DiscoveredFile& file : files)
        dto.entries.push_back(ToDto(file));

    return dto;
}

DirectoryResultDto BuildDirectoryResultDto(const RpcDirectoryProgressEvent& event)
{
    return BuildDirectoryResultDto(
        event.requestId,
        event.directoryPath,
        event.files,
        event.directories,
        event.finished);
}
