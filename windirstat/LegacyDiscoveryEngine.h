#pragma once

#include "DiscoveryBatch.h"
#include "LegacyDiscoveryRequest.h"

class IDirectoryDiscoveryEngine
{
public:
    virtual ~IDirectoryDiscoveryEngine() = default;
    // Performs discovery for exactly one processed directory and returns the
    // immediate discovered children for that directory.
    virtual DiscoveryBatch Discover(const LegacyDiscoveryRequest& request) = 0;
};

class LegacyDiscoveryEngine final : public IDirectoryDiscoveryEngine
{
public:
    DiscoveryBatch Discover(const LegacyDiscoveryRequest& request) override;
};
