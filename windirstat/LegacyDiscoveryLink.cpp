#include "pch.h"
#include "LegacyDiscoveryLink.h"

#include "DiscoveryBatch.h"
#include "LegacyDiscoveryRequest.h"
#include "ScanTask.h"
#include "LegacyDiscoveryExtractor.h"
#include "ItemDiscoverySink.h"

LegacyDiscoveryLink::LegacyDiscoveryLink(
    LegacyDiscoveryExtractor& extractor,
    CItemDiscoverySink& sink)
    : m_extractor(extractor)
    , m_sink(sink)
{
}

void LegacyDiscoveryLink::Process(const ScanTask& task, BlockingQueue<ScanTask>& queue)
{
    LegacyDiscoveryRequest request{};
    request.path = task.path;

    DiscoveryBatch batch = m_extractor.Extract(request);
    batch.scannedPath = task.path;

    auto childTasks = m_sink.Apply(batch);

    for (const auto& childTask : childTasks)
    {
        queue.Push(childTask);
    }
}
