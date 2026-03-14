#pragma once
#include <memory>
#include <string>

#include "ScanRequest.h"
#include "ScanResult.h"

class IScanEngine
{
public:
    virtual ~IScanEngine() = default;
    virtual std::unique_ptr<ScanResult> Scan(const ScanRequest& request) = 0;
};
