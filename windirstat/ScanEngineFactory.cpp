#include "pch.h"
#include "ScanEngineFactory.h"
#include "IScanEngine.h"
#include "LegacyScanEngine.h"

std::unique_ptr<IScanEngine> ScanEngineFactory::Create()
{
    return std::make_unique<LegacyScanEngine>();
}
