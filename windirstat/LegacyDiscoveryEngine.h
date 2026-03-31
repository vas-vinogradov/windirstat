#pragma once

#include "DiscoveryBatch.h"
#include "LegacyDiscoveryRequest.h"

class LegacyDiscoveryEngine
{
public:
    DiscoveryBatch Discover(const LegacyDiscoveryRequest& request);
};
