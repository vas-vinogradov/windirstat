#pragma once
#pragma once

class CItem;
class FinderNtfsContext;
class FinderBasicContext;

struct LegacyDiscoveryRequest
{
    CItem* item = nullptr;
    FinderNtfsContext* ntfsContext = nullptr;
    FinderBasicContext* basicContext = nullptr;
};
