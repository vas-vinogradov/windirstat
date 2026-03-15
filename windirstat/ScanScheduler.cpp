#include "pch.h"
#include "ScanScheduler.h"

#include "LegacyDiscoveryExtractor.h"
#include "LegacyDiscoveryRequest.h"
#include "IDiscoverySink.h"
#include "BlockingQueue.h"
#include "IDiscoveryLink.h"
#include "Item.h"
#include "ScanTask.h"
#include "BlockingQueue.h"

/*void ScanScheduler::Run(const ScanTask& rootTask, IDiscoveryLink& discoveryLink)
{
    BlockingQueue<ScanTask> queue;
    queue.Push(rootTask);

    while (true) {
        auto task = queue.Pop() */ /* pop next task */;
        /*if (!task.has_value()) {
            // No more tasks to process, exit the loop
            break;
        }
        discoveryLink.Process(task.value(), queue);
    }
}
*/
