#pragma once

#include <string>
#include <vector>
#include "ScanTask.h"
#include "DiscoveryBatch.h"

class IDiscoverySink
{
public:
    virtual ~IDiscoverySink() = default;

    virtual std::vector<ScanTask> Apply(const DiscoveryBatch& batch) = 0;

    // Called by the scan engine after one ScanTask has been fully processed
    // (batch extracted, applied, and child tasks queued).
    virtual void CompleteTask(const std::wstring& path) = 0;
};
