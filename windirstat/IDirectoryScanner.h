#pragma once
#include <string>
#include "DiscoveryBatch.h"
class IDirectoryScanner {
public:
    virtual ~IDirectoryScanner() = default;
    // Scans a single directory and returns a batch of discovered files and directories
    virtual DiscoveryBatch ScanDirectory(const std::wstring& directoryPath) = 0;
};
