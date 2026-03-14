#pragma once

#include "DiscoveryBatch.h"

class IDiscoverySink
{
public:
    virtual ~IDiscoverySink() = default;

    virtual void Apply(const DiscoveryBatch& batch) = 0;
};
