#pragma once

#include "LegacyDiscoveryEngine.h"

// Deterministic seam-validation implementation used only when explicitly
// selected for controlled remote-host runs.
class StubDirectoryDiscoveryEngine final : public IDirectoryDiscoveryEngine
{
public:
    DiscoveryBatch Discover(const LegacyDiscoveryRequest& request) override;
};
