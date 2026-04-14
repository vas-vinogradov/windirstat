#pragma once

#include "ScanRequest.h"
#include "ScanTerminalReason.h"

class IScanObserver;

class IScanEngine
{
public:
    virtual ~IScanEngine() = default;
    // Starts a new scan session, assigns a new authoritative requestId, and
    // enqueues the passed request as the initial work item.
    virtual void StartScan(const ScanRequest& request, IScanObserver& observer) = 0;
    // Enqueues follow-up work for the active scan session. Implementations must
    // reject stale work and requests when no active session exists.
    virtual void Enqueue(const ScanRequest& request) = 0;
    // Cancels the active scan session. The engine must later emit exactly one
    // terminal callback for the requestId: OnCompleted(...) or OnCanceled(...).
    virtual void Cancel(ScanTerminalReason reason = ScanTerminalReason::EngineInterrupted) = 0;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
    virtual bool IsRunning() const = 0;
    // Returns true when the engine owns recursive traversal internally after
    // the initial request inputs are submitted. Engines returning false still
    // expect the observer/UI side to enqueue followed child directories.
    virtual bool OwnsTraversal() const { return false; }
    // Returns the authoritative active requestId, or 0 when no active scan is
    // running. UI may use this to reject stale callbacks.
    virtual std::uint64_t GetActiveRequestId() const = 0;
};
