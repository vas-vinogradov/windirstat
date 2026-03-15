#pragma once

#include "DiscoveryBatch.h"

struct ScanTask;

class IDiscoverySink
{
public:
    virtual ~IDiscoverySink() = default;
     
    virtual  std::vector<ScanTask> Apply(const DiscoveryBatch& batch) = 0 ;
};
