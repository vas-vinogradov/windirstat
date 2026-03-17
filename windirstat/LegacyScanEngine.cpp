#include "pch.h"
#include "LegacyScanEngine.h"

#include "DiscoveryBatch.h"
#include "IDiscoverySink.h"
#include "LegacyDiscoveryRequest.h"
#include "ScanResult.h"
#include "ScanTask.h"

           
LegacyScanEngine::~LegacyScanEngine() = default;

void LegacyScanEngine::Scan(
    std::vector<ScanTask> rootTasks,
    IDiscoverySink& sink)
{
    BlockingQueue<ScanTask> queue;

    for (const auto& rootTask : rootTasks)
        queue.Push(rootTask);

    while (true)
    {
        auto task = queue.Pop();
        if (!task.has_value())
            break;

        const std::wstring scannedPath = task->path;

        LegacyDiscoveryRequest request{};
        request.path = scannedPath;
        request.ntfsContext = &m_contextNtfs;
        request.basicContext = &m_contextBasic;

        DiscoveryBatch batch = m_extractor.Extract(request);
        batch.scannedPath = scannedPath;

        // IMPORTANT:
        // Complete the current task only after all child tasks discovered from this batch
        // have been published to the queue. Calling CompleteTask() inside Apply()
        // can let parent m_jobs reach zero before child work is published.
        auto childTasks = sink.Apply(batch);



        for (const auto& childTask : childTasks)
            queue.Push(childTask);

        sink.CompleteTask(batch.scannedPath);
    }
}

