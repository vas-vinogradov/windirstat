#include "pch.h"
#include "ScanScheduler.h"

#include "LegacyDiscoveryExtractor.h"
#include "LegacyDiscoveryRequest.h"
#include "IDiscoverySink.h"
#include "BlockingQueue.h"
#include "Item.h"

void ScanScheduler::Run(
    BlockingQueue<CItem*>& queue,
    FinderNtfsContext& contextNtfs,
    FinderBasicContext& contextBasic,
    IDiscoverySink& sink)
{
    LegacyDiscoveryExtractor extractor;

    for (auto itemOpt = queue.Pop(); itemOpt.has_value(); itemOpt = queue.Pop()) {
        CItem* const item = itemOpt.value();

        LegacyDiscoveryRequest request;
        request.path = item->GetPath();
        request.index = item->GetIndex();
        request.attributes = item->GetAttributes();
        request.forceBasic = item->IsTypeOrFlag(ITF_BASIC);
        request.ntfsContext = &contextNtfs;
        request.basicContext = &contextBasic;

        DiscoveryBatch batch = extractor.Extract(request);

        sink.Apply(*item, batch);
    }
}
