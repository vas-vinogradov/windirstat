#include "pch.h"
#include "LegacyScanEngine.h"

#include "DiscoveryBatch.h"
#include "IDiscoverySink.h"
#include "LegacyDiscoveryRequest.h"
#include "ScanResult.h"
#include "ScanTask.h"

           
LegacyScanEngine::~LegacyScanEngine() = default;

void LegacyScanEngine::Scan(std::vector<ScanTask> rootTasks, IDiscoverySink& sink)
{
    BlockingQueue<ScanTask> queue;
    
    for (const auto& rootTask : rootTasks)
    {
        queue.Push(rootTask);
    }
    

    while (true)
    {
        auto task = queue.Pop();
        if (!task.has_value())
            break;

        LegacyDiscoveryRequest request{};
        request.path = task->path;
        request.ntfsContext = &m_contextNtfs;
        request.basicContext = &m_contextBasic;

        DiscoveryBatch batch = m_extractor.Extract(request);
        batch.scannedPath = task->path;

        auto nextTasks = sink.Apply(batch);

        for (const auto& nextTask : nextTasks)
            queue.Push(nextTask);
    }
}
