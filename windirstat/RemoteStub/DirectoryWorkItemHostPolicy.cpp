#include "pch.h"
#include "RemoteStub/DirectoryWorkItemHostPolicy.h"

DirectoryWorkItemPolicyResult DirectoryWorkItemHostPolicy::Apply(DirectoryWorkItemPolicyInput input) const
{
    if (const auto* failure = std::get_if<DirectoryWorkItemFailure>(&input.workItemResult.outcome))
    {
        DirectoryFailurePayload failurePayload{};
        failurePayload.path = std::move(input.workItemResult.path);
        failurePayload.message = failure->message;
        failurePayload.errorCode = failure->errorCode;
        return DirectoryWorkItemPolicyResult{ std::move(failurePayload) };
    }

    auto& discoveryBatch = std::get<DirectoryWorkItemSuccess>(input.workItemResult.outcome).discoveryBatch;
    DirectoryProgressPayload progress{};
    progress.directoryPath = std::move(input.workItemResult.path);
    progress.finished = true;

    progress.files = std::move(discoveryBatch.files);
    progress.directories = std::move(discoveryBatch.directories);
    return DirectoryWorkItemPolicyResult{ std::move(progress) };
}
