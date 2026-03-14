#pragma once

#include <memory>

class IScanEngine;

class ScanEngineFactory
{
public:
    static std::unique_ptr<IScanEngine> Create();
};
