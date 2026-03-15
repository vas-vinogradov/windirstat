#pragma once

#include "IDiscoverySink.h"
#include "IScanEngine.h"
#include "LegacyDiscoveryExtractor.h"
#include "FinderNtfs.h"
#include "FinderBasic.h"


class LegacyScanEngine : public IScanEngine
{
public:
    LegacyScanEngine() = default;
    ~LegacyScanEngine() override;

public:

    void Scan(std::vector<ScanTask> rootTasks, IDiscoverySink& sink) override;

private:
    FinderNtfsContext m_contextNtfs{};
    FinderBasicContext m_contextBasic{};
    LegacyDiscoveryExtractor m_extractor{};
};
