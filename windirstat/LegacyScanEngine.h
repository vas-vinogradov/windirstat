#pragma once

#include "IScanEngine.h"

class LegacyScanEngine : public IScanEngine
{
public:
    LegacyScanEngine();
    ~LegacyScanEngine() override;

    std::unique_ptr<ScanResult> Scan(const ScanRequest& request) override;
};
