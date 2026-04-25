#pragma once

#include "UiScanDtos.h"

struct DiscoveredDirectory;
struct DiscoveredFile;
struct RpcDirectoryProgressEvent;

DirectoryResultDto BuildDirectoryResultDto(
    std::uint64_t requestId,
    std::wstring directoryPath,
    std::vector<DiscoveredFile> files,
    std::vector<DiscoveredDirectory> directories,
    bool finished);

DirectoryResultDto BuildDirectoryResultDto(const RpcDirectoryProgressEvent& event);
