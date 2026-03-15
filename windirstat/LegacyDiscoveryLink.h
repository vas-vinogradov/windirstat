#pragma once

#include "IDiscoveryLink.h"

class LegacyDiscoveryExtractor;
class CItemDiscoverySink;

class LegacyDiscoveryLink : public IDiscoveryLink
{
public:
    LegacyDiscoveryLink(
        LegacyDiscoveryExtractor& extractor,
        CItemDiscoverySink& sink);

    void Process(const ScanTask& task, BlockingQueue<ScanTask>& queue) override;

private:
    LegacyDiscoveryExtractor& m_extractor;
    CItemDiscoverySink& m_sink;
};
