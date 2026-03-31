#pragma once

#include "DirectoryProgressBatch.h"
#include "ScanError.h"
#include "ScanTerminalReason.h"

class IScanObserver
{
public:
    virtual ~IScanObserver() = default;

    // Callbacks are request-scoped and may arrive on arbitrary threads.
    // UI must ignore stale callbacks for inactive requestIds.
    // UI may enqueue follow-up work in response to progress callbacks.
    virtual void OnDirectoryProgress(DirectoryProgressBatch batch) = 0;
    // Successful terminal event. Exactly one terminal callback must be emitted
    // per requestId, and it is the last callback for that requestId.
    virtual void OnCompleted(std::uint64_t requestId) = 0;
    // Non-success terminal event, distinct from OnError(...).
    virtual void OnCanceled(std::uint64_t requestId, ScanTerminalReason reason) = 0;
    // Per-directory, non-fatal error notification. Multiple OnError callbacks
    // may occur during a scan, and a scan may still later end with OnCompleted.
    virtual void OnError(const ScanError& error) = 0;
};
