#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DiscoveredDirectory.h"
#include "DiscoveredFile.h"

struct DirectoryProgressBatch
{
    std::uint64_t requestId = 0;
    // Local observer-facing form of RpcDirectoryProgressEvent. This is an
    // authoritative snapshot of one directory's immediate discovered children.
    std::wstring directoryPath;
    std::vector<DiscoveredFile> files;
    std::vector<DiscoveredDirectory> directories;
    // finished=true means the directory snapshot is complete enough that the
    // client may remove previously known children omitted from this batch.
    bool finished = false;
};
