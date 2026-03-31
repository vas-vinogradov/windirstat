#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DiscoveredDirectory.h"
#include "DiscoveredFile.h"

struct DirectoryProgressBatch
{
    std::uint64_t requestId = 0;
    std::wstring directoryPath;
    std::vector<DiscoveredFile> files;
    std::vector<DiscoveredDirectory> directories;
    bool finished = false;
};
