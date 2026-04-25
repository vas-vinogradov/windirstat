#include "pch.h"
#include "RemoteStub/DirectoryWorkItemProcessor.h"

DiscoveryDirectoryWorkItemProcessor::DiscoveryDirectoryWorkItemProcessor(std::unique_ptr<IDirectoryDiscoveryEngine> discoveryEngine)
    : m_discoveryEngine(std::move(discoveryEngine))
{
}

DirectoryWorkItemResult DiscoveryDirectoryWorkItemProcessor::Process(const std::wstring& path)
{
    DirectoryWorkItemResult result{};
    result.path = path;

    try
    {
        DirectoryDiscoveryRequest request{};
        request.path = path;

        ASSERT(m_discoveryEngine != nullptr);
        result.outcome = DirectoryWorkItemSuccess{ m_discoveryEngine->Discover(request) };
    }
    catch (const std::exception& ex)
    {
        DirectoryWorkItemFailure failure{};
        failure.message = std::wstring(CA2W(ex.what()));
        failure.errorCode = 0;
        result.outcome = std::move(failure);
    }

    return result;
}
