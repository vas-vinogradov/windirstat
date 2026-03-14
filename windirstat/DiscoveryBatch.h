#pragma once
#include <string>
#include <vector>
#include "DiscoveredFile.h"
#include "DiscoveredDirectory.h"
struct DiscoveryBatch {
    std::wstring scannedPath;                        // The path that was scanned
    std::vector<DiscoveredFile> files;              // Discovered files in the directory
    std::vector<DiscoveredDirectory> directories;   // Discovered directories in the directory
};
