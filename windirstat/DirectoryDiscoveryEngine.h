#pragma once

#include "DiscoveryBatch.h"

#include <string>

// Shared discovery seam for one host-owned work unit. The caller owns traversal,
// scheduling, RPC, and lifecycle; implementations enumerate only this path's
// immediate children and return contract-level metadata in DiscoveryBatch.
struct DirectoryDiscoveryRequest
{
    std::wstring path;
};

class IDirectoryDiscoveryEngine
{
public:
    virtual ~IDirectoryDiscoveryEngine() = default;
    virtual DiscoveryBatch Discover(const DirectoryDiscoveryRequest& request) = 0;
};
