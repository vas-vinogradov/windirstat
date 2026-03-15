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

        auto childTasks = sink.Apply(batch);

        for (const auto& childTask : childTasks)
            queue.Push(childTask);

        // Critical missing completion boundary from old scan loop
        sink.CompleteTask(scannedPath);
    }
}

