#pragma once

#include "FinderBasic.h"
#include "FinderNtfs.h"
#include "LegacyDiscoveryEngine.h"

#include <string>

class FakeRemoteScanHost
{
public:
    int Run(const std::wstring& pipeName);

private:
    bool ExecuteDiscovery(class NamedPipeRpcServer& server, std::uint64_t requestId, const std::wstring& path);

    FinderNtfsContext m_contextNtfs{};
    FinderBasicContext m_contextBasic{};
    LegacyDiscoveryEngine m_discoveryEngine{};
};
