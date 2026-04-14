#pragma once

#include "DirectoryDiscoveryEngine.h"

// First real non-legacy discovery implementation behind IDirectoryDiscoveryEngine.
// It enumerates the immediate children of exactly one requested directory and
// returns one authoritative per-directory snapshot for the existing RPC contract.
class BasicReplacementDiscoveryEngine final : public IDirectoryDiscoveryEngine
{
public:
    DiscoveryBatch Discover(const DirectoryDiscoveryRequest& request) override;
};
