#pragma once

#include <memory>

class CItem;
struct ScanResult;

class ScanResultToCItemMapper
{
public:
    static std::unique_ptr<CItem> Map(const ScanResult& result);
};
