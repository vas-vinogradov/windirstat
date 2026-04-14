#pragma once

#include "DirectoryDiscoveryEngine.h"

// Deterministic seam-validation implementation used only when explicitly
// selected for controlled remote-host runs.
class StubDirectoryDiscoveryEngine final : public IDirectoryDiscoveryEngine
{
public:
    DiscoveryBatch Discover(const DirectoryDiscoveryRequest& request) override;
};
