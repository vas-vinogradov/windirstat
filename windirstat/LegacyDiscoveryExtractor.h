#pragma once

#include "DiscoveryBatch.h"
#include "LegacyDiscoveryRequest.h"

class LegacyDiscoveryExtractor
{
public:
    DiscoveryBatch Extract(const LegacyDiscoveryRequest& request);
};
