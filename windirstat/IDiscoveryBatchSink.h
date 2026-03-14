#pragma once

#include "DiscoveryBatch.h"

class IDiscoveryBatchSink
{
public:
    virtual ~IDiscoveryBatchSink() = default;
    virtual void Apply(CItem& item, const DiscoveryBatch& batch) = 0;
    virtual void OnDiscoveryBatch(const DiscoveryBatch& batch) = 0;
};
