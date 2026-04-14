#include "pch.h"
#include "LegacyScanEngine.h"

#include "BlockingQueue.h"
#include "Options.h"
#include "DirectoryProgressBatch.h"
#include "IScanObserver.h"

class LegacyScanEngine::ScanJob
{
public:
    ScanJob(
        LegacyScanEngine& engine,
        IScanObserver& observer,
        const std::uint64_t requestId)
        : m_engine(engine)
        , m_observer(observer)
        , m_requestId(requestId)
    {
        m_queue.StartThreads(COptions::ScanningThreads, [this]()
        {
            WorkerLoop();
        });

        m_completionThread.emplace([this]()
        {
            const int stopReason = m_queue.WaitForCompletion();
            m_running = false;

            if (stopReason != 0)
            {
                m_observer.OnCanceled(m_requestId, static_cast<ScanTerminalReason>(stopReason));
                return;
            }

            m_observer.OnCompleted(m_requestId);
        });
    }

    ~ScanJob()
    {
        Cancel(ScanTerminalReason::EngineInterrupted);
    }

    void Enqueue(const ScanRequest& request)
    {
        if (request.requestId != m_requestId)
            return;

        m_queue.Push(request);
    }

    void Cancel(const ScanTerminalReason reason)
    {
        if (!m_running.exchange(false))
            return;

        m_queue.CancelExecution(static_cast<int>(reason));
        if (m_completionThread.has_value() && m_completionThread->joinable())
            m_completionThread->join();
        m_completionThread.reset();
    }

    void Suspend()
    {
        m_queue.SuspendExecution();
    }

    void Resume()
    {
        m_queue.ResumeExecution();
    }

    bool IsRunning() const
    {
        return m_running.load(std::memory_order_acquire);
    }

    std::uint64_t GetRequestId() const
    {
        return m_requestId;
    }

private:
    void WorkerLoop()
    {
        while (true)
        {
            auto requestOpt = m_queue.Pop();
            if (!requestOpt.has_value())
                break;

            ScanRequest request = std::move(requestOpt.value());
            if (request.requestId != m_requestId)
                continue;

            m_engine.ProcessDirectory(request, m_observer);
        }
    }

    LegacyScanEngine& m_engine;
    IScanObserver& m_observer;
    const std::uint64_t m_requestId;
    BlockingQueue<ScanRequest> m_queue;
    std::optional<std::jthread> m_completionThread;
    std::atomic_bool m_running = true;
};

LegacyScanEngine::LegacyScanEngine() = default;

LegacyScanEngine::~LegacyScanEngine() = default;

void LegacyScanEngine::ProcessDirectory(
    const ScanRequest& request,
    IScanObserver& observer)
{
    try
    {
        DirectoryDiscoveryRequest discoveryRequest{};
        discoveryRequest.path = request.rootPath;

        DiscoveryBatch discoveryBatch = m_discoveryEngine.Discover(discoveryRequest);

        DirectoryProgressBatch progressBatch{};
        progressBatch.requestId = request.requestId;
        progressBatch.directoryPath = request.rootPath;
        progressBatch.files = std::move(discoveryBatch.files);
        progressBatch.directories = std::move(discoveryBatch.directories);
        progressBatch.finished = true;
        observer.OnDirectoryProgress(std::move(progressBatch));
    }
    catch (const std::exception& ex)
    {
        ScanError error{};
        error.requestId = request.requestId;
        error.path = request.rootPath;
        error.message = std::wstring(CA2W(ex.what()));
        error.errorCode = 0;
        observer.OnError(error);
        return;
    }
}

void LegacyScanEngine::StartScan(
    const ScanRequest& request,
    IScanObserver& observer)
{
    const std::uint64_t requestId = m_nextRequestId.fetch_add(1, std::memory_order_acq_rel);
    m_activeRequestId.store(requestId, std::memory_order_release);

    ScanRequest initialRequest = request;
    initialRequest.requestId = requestId;

    std::unique_ptr<ScanJob> oldJob;
    {
        std::scoped_lock lock(m_jobMutex);
        oldJob = std::move(m_job);
        m_job = std::make_unique<ScanJob>(*this, observer, requestId);
        m_job->Enqueue(initialRequest);
    }

    if (oldJob != nullptr)
        oldJob->Cancel(ScanTerminalReason::Restarted);
}

void LegacyScanEngine::Enqueue(const ScanRequest& request)
{
    std::scoped_lock lock(m_jobMutex);
    if (m_job == nullptr || !m_job->IsRunning())
        return;

    const std::uint64_t activeRequestId = m_activeRequestId.load(std::memory_order_acquire);
    if (activeRequestId == 0)
        return;

    if (request.requestId != 0 && request.requestId != activeRequestId)
        return;

    ScanRequest queuedRequest = request;
    queuedRequest.requestId = activeRequestId;
    m_job->Enqueue(queuedRequest);
}

void LegacyScanEngine::Cancel(const ScanTerminalReason reason)
{
    std::unique_ptr<ScanJob> job;
    {
        std::scoped_lock lock(m_jobMutex);
        job = std::move(m_job);
    }

    if (job != nullptr)
        job->Cancel(reason);
}

void LegacyScanEngine::Suspend()
{
    std::scoped_lock lock(m_jobMutex);
    if (m_job != nullptr)
        m_job->Suspend();
}

void LegacyScanEngine::Resume()
{
    std::scoped_lock lock(m_jobMutex);
    if (m_job != nullptr)
        m_job->Resume();
}

bool LegacyScanEngine::IsRunning() const
{
    std::scoped_lock lock(m_jobMutex);
    return m_job != nullptr && m_job->IsRunning();
}

std::uint64_t LegacyScanEngine::GetActiveRequestId() const
{
    return m_activeRequestId.load(std::memory_order_acquire);
}
