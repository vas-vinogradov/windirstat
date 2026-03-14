#pragma once

#include "DiscoveryBatch.h"

class IDiscoveryBatchSink
{
public:
    virtual ~IDiscoveryBatchSink() = default;

    virtual void OnDiscoveryBatch(const DiscoveryBatch& batch) = 0;
};
