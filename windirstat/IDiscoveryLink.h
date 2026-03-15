#pragma once

template <typename T>
class BlockingQueue;

struct ScanTask;

class IDiscoveryLink
{
public:
    virtual ~IDiscoveryLink() = default;
    virtual void Process(const ScanTask& task, BlockingQueue<ScanTask>& queue) = 0;
};

