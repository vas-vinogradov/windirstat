#pragma once

#include "DirectoryDiscoveryEngine.h"
#include "FinderBasic.h"
#include "FinderNtfs.h"

class LegacyDiscoveryEngine final : public IDirectoryDiscoveryEngine
{
public:
    DiscoveryBatch Discover(const DirectoryDiscoveryRequest& request) override;

private:
    FinderNtfsContext m_contextNtfs{};
    FinderBasicContext m_contextBasic{};
};
