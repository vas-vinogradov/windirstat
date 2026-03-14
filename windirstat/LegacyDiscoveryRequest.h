#pragma once
#pragma once

class CItem;
class FinderNtfsContext;
class FinderBasicContext;

struct LegacyDiscoveryRequest
{
    std::wstring path;
    ULONGLONG index = 0;
    DWORD attributes = 0;

    bool forceBasic = false;

    FinderNtfsContext* ntfsContext = nullptr;
    FinderBasicContext* basicContext = nullptr;
};
