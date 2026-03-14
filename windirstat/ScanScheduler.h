#pragma once

class CItem;
template<typename T>
class BlockingQueue;

class FinderNtfsContext;
class FinderBasicContext;
class IDiscoverySink;

class ScanScheduler
{
public:
    void Run(
        BlockingQueue<CItem*>& queue,
        FinderNtfsContext& contextNtfs,
        FinderBasicContext& contextBasic,
        IDiscoverySink& sink);
};
