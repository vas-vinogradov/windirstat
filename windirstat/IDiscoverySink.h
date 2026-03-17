#pragma once

#include <string>
#include <vector>
#include "ScanTask.h"
#include "DiscoveryBatch.h"

class IDiscoverySink
{
public:
    virtual ~IDiscoverySink() = default;

    // Applies the discovered batch to the internal state and returns new scan tasks for discovered directories.
    virtual std::vector<ScanTask> Apply(const DiscoveryBatch& batch) = 0;

    //Is the only way to complete jobs.
    virtual void CompleteTask(const std::wstring& path) = 0;
};
