#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "IDirectoryScanner.h"
#include "IDiscoveryBatchSink.h"
#include "ScanTask.h"

class ScanScheduler
{
public:
    ScanScheduler(
        IDirectoryScanner& scanner,
        IDiscoveryBatchSink& batchSink,
        size_t maxThreads = std::thread::hardware_concurrency());

    ~ScanScheduler();

    void EnqueueTask(const ScanTask& task);
    void Start();
    void Stop();
    void WaitUntilComplete();
    bool IsComplete() const;

private:
    void WorkerThread();

    IDirectoryScanner& m_scanner;
    IDiscoveryBatchSink& m_batchSink;

    std::queue<ScanTask> m_taskQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;

    std::vector<std::thread> m_workers;

    std::atomic<bool> m_running{ false };
    std::atomic<size_t> m_pendingTaskCount{ 0 };

    size_t m_maxThreads = 0;
};
