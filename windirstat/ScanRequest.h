#pragma once

#include <cstdint>
#include <string>

struct ScanRequest
{
    std::uint64_t requestId = 0;
    std::wstring rootPath;
    bool expectMoreInputs = false;
};
