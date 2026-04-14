#pragma once

#include "IScanEngine.h"
#include "LegacyDiscoveryEngine.h"

#include <atomic>
#include <memory>
#include <mutex>

class LegacyScanEngine : public IScanEngine
{
public:
    LegacyScanEngine();
    ~LegacyScanEngine() override;

    void StartScan(const ScanRequest& request, IScanObserver& observer) override;
    void Enqueue(const ScanRequest& request) override;
    void Cancel(ScanTerminalReason reason = ScanTerminalReason::EngineInterrupted) override;
    void Suspend() override;
    void Resume() override;
    bool IsRunning() const override;
    std::uint64_t GetActiveRequestId() const override;

private:
    class ScanJob;

    void ProcessDirectory(const ScanRequest& request, IScanObserver& observer);

    mutable std::mutex m_jobMutex;
    std::unique_ptr<ScanJob> m_job;
    std::atomic_uint64_t m_nextRequestId = 1;
    std::atomic_uint64_t m_activeRequestId = 0;
    LegacyDiscoveryEngine m_discoveryEngine{};
};
