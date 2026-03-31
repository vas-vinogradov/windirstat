#pragma once

#include <cstdint>
#include <string>

struct ScanError
{
    std::uint64_t requestId = 0;
    std::wstring path;
    std::wstring message;
    unsigned long errorCode = 0;
};
