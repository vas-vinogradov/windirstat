#pragma once
#include <memory>
#include <string>

#include "ScanRequest.h"
#include "ScanResult.h"
#include "ScanTask.h"

class IDiscoverySink;

class IScanEngine
{
public:
    virtual ~IScanEngine() = default;
    virtual void Scan(std::vector<ScanTask>, IDiscoverySink& sink) = 0;
};
