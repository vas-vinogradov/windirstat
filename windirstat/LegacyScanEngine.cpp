#include "pch.h"
#include "LegacyScanEngine.h"

#include "ScanResult.h"

LegacyScanEngine::LegacyScanEngine() = default;
LegacyScanEngine::~LegacyScanEngine() = default;

std::unique_ptr<ScanResult> LegacyScanEngine::Scan(const ScanRequest& request)
{
    (void)request;
    return std::make_unique<ScanResult>();
}
