#include "pch.h"

#include "Engine/Rpc/RpcMessages.h"
#include "RemoteStub/ScanSessionState.h"
#include "UiScanProjectionPlan.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
void Check(const bool condition, const std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

ScanSessionOptions DefaultOptions()
{
    return {};
}

TraversalChildCandidate Candidate(const std::wstring& fullPath, const std::uint32_t reparseTag = 0, const bool protectedReparsePoint = false)
{
    TraversalChildCandidate candidate{};
    candidate.fullPath = fullPath;
    candidate.reparseTag = reparseTag;
    candidate.isProtectedReparsePoint = protectedReparsePoint;
    return candidate;
}

ScanSessionTerminalDecision TakeRequiredDecision(ScanSessionState& state)
{
    const auto decision = state.TryTakeTerminalDecision();
    Check(decision.has_value(), "expected terminal decision");
    return decision.value();
}

ScanSessionAction RequireAction(ScanSessionState& state)
{
    return state.NextAction();
}

void MultiStepScanProgressesToCompletion()
{
    ScanSessionState state;
    state.Start(L"C:\\root", DefaultOptions());

    Check(state.PendingWorkCount() == 1, "start queues root");
    Check(RequireAction(state).path == L"C:\\root", "root is first path");

    const ScanSessionDirectoryReport rootReport = state.ReportDirectoryResult({
        Candidate(L"C:\\root\\first"),
        Candidate(L"C:\\root\\second"),
    });
    Check(rootReport.scheduledChildPaths.size() == 2, "root schedules two child directories");
    Check(state.OutstandingWorkCount() == 2, "scheduled children replace completed root work");

    Check(RequireAction(state).path == L"C:\\root\\first", "first child is next");
    const ScanSessionDirectoryReport firstReport = state.ReportDirectoryResult({
        Candidate(L"C:\\root\\first\\leaf"),
    });
    Check(firstReport.scheduledChildPaths.size() == 1, "first child schedules one grandchild");

    Check(RequireAction(state).path == L"C:\\root\\second", "second child is next");
    Check(state.ReportDirectoryResult({}).scheduledChildPaths.empty(), "empty second child schedules nothing");

    state.CloseInput();
    Check(!state.HasTerminalState(), "closed input waits for remaining grandchild");
    Check(RequireAction(state).path == L"C:\\root\\first\\leaf", "grandchild drains last");
    state.ReportDirectoryResult({});

    Check(state.HasTerminalState(), "session completes after input closed and work drains");
    Check(state.NextAction().kind == ScanSessionActionKind::Complete, "completed action is surfaced");
    Check(TakeRequiredDecision(state).kind == ScanSessionTerminalKind::Completed, "terminal decision is completed");
    Check(!state.TryTakeTerminalDecision().has_value(), "terminal decision is taken once");
}

void EmptyResultCompletesAfterCloseInput()
{
    ScanSessionState state;
    state.Start(L"C:\\empty", DefaultOptions());

    Check(RequireAction(state).path == L"C:\\empty", "root is processed");
    Check(state.ReportDirectoryResult({}).scheduledChildPaths.empty(), "empty directory schedules no children");
    Check(!state.HasTerminalState(), "empty result does not complete until input closes");
    Check(state.NextAction().kind == ScanSessionActionKind::WaitingForInput, "drained queue waits for more input");

    state.CloseInput();

    Check(state.HasTerminalState(), "closed empty session completes");
    Check(TakeRequiredDecision(state).kind == ScanSessionTerminalKind::Completed, "empty terminal is completed");
}

void FailureClearsPendingWorkAndWins()
{
    ScanSessionState state;
    state.Start(L"C:\\root", DefaultOptions());
    Check(RequireAction(state).path == L"C:\\root", "root is processed");
    state.ReportDirectoryResult({ Candidate(L"C:\\root\\child") });
    Check(state.PendingWorkCount() == 1, "child is pending before failure");

    ScanSessionFailure failure;
    failure.path = L"C:\\root\\child";
    failure.message = L"boom";
    failure.errorCode = 7;
    state.ReportDirectoryFailure(std::move(failure));

    Check(state.PendingWorkCount() == 0, "failure clears pending work");
    state.RequestCancel(ScanTerminalReason::UserCancel);
    Check(state.NextAction().kind == ScanSessionActionKind::Failed, "failed action is surfaced");
    const ScanSessionTerminalDecision decision = TakeRequiredDecision(state);
    Check(decision.kind == ScanSessionTerminalKind::Failed, "failure remains terminal decision");
    Check(decision.failure.path == L"C:\\root\\child", "failure path is preserved");
}

void CancellationClearsQueueAndIgnoresLateResults()
{
    ScanSessionState state;
    state.Start(L"C:\\root", DefaultOptions());
    Check(RequireAction(state).path == L"C:\\root", "root is processed");
    state.ReportDirectoryResult({ Candidate(L"C:\\root\\child") });
    Check(state.PendingWorkCount() == 1, "child is pending before cancel");

    state.RequestCancel(ScanTerminalReason::UserCancel);

    Check(state.PendingWorkCount() == 0, "cancel clears pending work");
    Check(state.IsCancellationRequested(), "cancel flag is recorded");
    Check(state.IsInputClosed(), "cancel closes input");
    Check(state.ReportDirectoryResult({ Candidate(L"C:\\late") }).scheduledChildPaths.empty(),
        "late result after cancel schedules nothing");
    Check(state.NextAction().kind == ScanSessionActionKind::Cancelled, "canceled action is surfaced");

    const ScanSessionTerminalDecision decision = TakeRequiredDecision(state);
    Check(decision.kind == ScanSessionTerminalKind::Canceled, "terminal decision is canceled");
    Check(decision.cancelReason == ScanTerminalReason::UserCancel, "cancel reason is preserved");
}

void ExternalEnqueueIsTrackedBySession()
{
    ScanSessionState state;
    state.Start(L"C:\\root", DefaultOptions());

    Check(state.AddExternalPath(L"C:\\extra"), "external path is accepted");
    Check(!state.AddExternalPath(L"C:\\extra"), "duplicate external path is ignored");
    Check(state.PendingWorkCount() == 2, "root plus external path are pending");
    Check(RequireAction(state).path == L"C:\\root", "root remains first");
    state.ReportDirectoryResult({});
    Check(RequireAction(state).path == L"C:\\extra", "external path is processed");
    state.ReportDirectoryResult({});
    state.CloseInput();
    Check(TakeRequiredDecision(state).kind == ScanSessionTerminalKind::Completed, "external enqueue completes normally");
}

void TraversalPolicyStaysInSession()
{
    ScanSessionState state;
    state.Start(L"C:\\root", DefaultOptions());

    Check(RequireAction(state).path == L"C:\\root", "root is processed");
    const ScanSessionDirectoryReport report = state.ReportDirectoryResult({
        Candidate(L"C:\\root\\normal"),
        Candidate(L"C:\\root\\link", IO_REPARSE_TAG_SYMLINK, false),
        Candidate(L"C:\\root\\protected", 0, true),
    });

    Check(report.scheduledChildPaths.size() == 1, "only normal directory is scheduled");
    Check(report.scheduledChildPaths.front() == L"C:\\root\\normal", "normal directory remains eligible");
}

void ResetRestoresCleanSession()
{
    ScanSessionState state;
    state.Start(L"C:\\root", DefaultOptions());
    state.RequestCancel(ScanTerminalReason::Restarted);
    Check(state.HasTerminalState(), "session is terminal before reset");

    state.Reset();

    Check(!state.HasTerminalState(), "reset clears terminal state");
    Check(!state.IsInputClosed(), "reset reopens input state");
    Check(!state.IsCancellationRequested(), "reset clears cancellation");
    Check(state.PendingWorkCount() == 0, "reset clears pending work");
}

RpcDirectoryProgressEvent MakeProgressEvent(const std::uint64_t requestId, const std::wstring& path)
{
    RpcDirectoryProgressEvent event{};
    event.requestId = requestId;
    event.directoryPath = path;
    event.finished = true;

    DiscoveredFile file{};
    file.name = L"alpha.txt";
    file.fullPath = path + L"\\alpha.txt";
    file.sizeLogical = 5;
    file.sizePhysical = 4096;
    event.files.push_back(std::move(file));
    return event;
}

void RpcDirectoryProgressBatchRoundTrips()
{
    RpcDirectoryProgressBatchEvent batch{};
    batch.requestId = 42;
    batch.items.push_back(MakeProgressEvent(42, L"C:\\root"));
    batch.items.push_back(MakeProgressEvent(42, L"C:\\root\\child"));

    const std::string json = SerializeRpcEventMessage(RpcEventMessage(batch));
    Check(json.find("DirectoryProgressBatchEvent") != std::string::npos, "batch event kind is serialized");

    const auto messageOpt = TryDeserializeRpcEventMessage(json);
    Check(messageOpt.has_value(), "batch event deserializes");
    const auto* parsed = std::get_if<RpcDirectoryProgressBatchEvent>(&messageOpt.value());
    Check(parsed != nullptr, "deserialized event is a batch");
    Check(parsed->requestId == 42, "batch request id survives");
    Check(parsed->items.size() == 2, "batch item count survives");
    Check(parsed->items[0].directoryPath == L"C:\\root", "first item path survives");
    Check(parsed->items[1].directoryPath == L"C:\\root\\child", "second item path survives");
    Check(parsed->items[0].files.size() == 1, "item payload survives");
}

void RpcDirectoryProgressEventStillRoundTrips()
{
    RpcDirectoryProgressEvent event = MakeProgressEvent(7, L"C:\\single");

    const std::string json = SerializeRpcEventMessage(RpcEventMessage(event));
    Check(json.find("DirectoryProgressEvent") != std::string::npos, "individual event kind is serialized");

    const auto messageOpt = TryDeserializeRpcEventMessage(json);
    Check(messageOpt.has_value(), "individual event deserializes");
    const auto* parsed = std::get_if<RpcDirectoryProgressEvent>(&messageOpt.value());
    Check(parsed != nullptr, "deserialized event remains individual progress");
    Check(parsed->requestId == 7, "individual request id survives");
    Check(parsed->directoryPath == L"C:\\single", "individual path survives");
    Check(parsed->files.size() == 1, "individual payload survives");
}

UiScanEntryDto MakeUiEntry(const UiScanEntryType type, const std::wstring& name, const std::wstring& parent = L"C:\\root")
{
    UiScanEntryDto entry{};
    entry.type = type;
    entry.name = name;
    entry.fullPath = parent + L"\\" + name;
    entry.sizeLogical = type == UiScanEntryType::File ? 10 : 0;
    entry.sizePhysical = type == UiScanEntryType::File ? 4096 : 0;
    entry.fileIndex = 0;
    return entry;
}

DirectoryResultDto MakeDirectoryResult(std::vector<UiScanEntryDto> entries, const bool finished = true)
{
    DirectoryResultDto result{};
    result.requestId = 77;
    result.path = L"C:\\root";
    result.entries = std::move(entries);
    result.finished = finished;
    return result;
}

bool ContainsKey(const std::vector<std::wstring>& keys, const std::wstring& key)
{
    return std::ranges::find(keys, key) != keys.end();
}

void UiProjectionPlansBasicDirectorySnapshot()
{
    const DirectoryResultDto result = MakeDirectoryResult({
        MakeUiEntry(UiScanEntryType::Directory, L"child"),
        MakeUiEntry(UiScanEntryType::File, L"alpha.txt"),
    });

    const UiScanProjectionPlan plan = BuildUiScanProjectionPlan(result, {});
    Check(plan.childKeysToRemove.empty(), "empty parent removes nothing");
    Check(plan.directoriesToProject.size() == 1, "one directory is projected");
    Check(plan.filesToProject.size() == 1, "one file is projected");
    Check(plan.directoriesToProject[0]->name == L"child", "directory name survives");
    Check(plan.filesToProject[0]->sizePhysical == 4096, "file size survives");
}

void UiProjectionFinishedReconcilesMissingChildren()
{
    const DirectoryResultDto result = MakeDirectoryResult({
        MakeUiEntry(UiScanEntryType::Directory, L"A"),
        MakeUiEntry(UiScanEntryType::File, L"B.txt"),
    }, true);
    const std::vector<std::wstring> existing = {
        BuildUiScanChildKey(UiScanEntryType::Directory, L"A"),
        BuildUiScanChildKey(UiScanEntryType::File, L"B.txt"),
        BuildUiScanChildKey(UiScanEntryType::File, L"C.txt"),
    };

    const UiScanProjectionPlan plan = BuildUiScanProjectionPlan(result, existing);
    Check(ContainsKey(plan.childKeysToRemove, BuildUiScanChildKey(UiScanEntryType::Directory, L"A")), "existing A is replaced");
    Check(ContainsKey(plan.childKeysToRemove, BuildUiScanChildKey(UiScanEntryType::File, L"B.txt")), "existing B is replaced");
    Check(ContainsKey(plan.childKeysToRemove, BuildUiScanChildKey(UiScanEntryType::File, L"C.txt")), "finished removes missing C");
}

void UiProjectionUnfinishedKeepsMissingChildren()
{
    const DirectoryResultDto result = MakeDirectoryResult({
        MakeUiEntry(UiScanEntryType::Directory, L"A"),
        MakeUiEntry(UiScanEntryType::File, L"B.txt"),
    }, false);
    const std::vector<std::wstring> existing = {
        BuildUiScanChildKey(UiScanEntryType::Directory, L"A"),
        BuildUiScanChildKey(UiScanEntryType::File, L"B.txt"),
        BuildUiScanChildKey(UiScanEntryType::File, L"C.txt"),
    };

    const UiScanProjectionPlan plan = BuildUiScanProjectionPlan(result, existing);
    Check(ContainsKey(plan.childKeysToRemove, BuildUiScanChildKey(UiScanEntryType::Directory, L"A")), "existing A is replaced before incremental update");
    Check(ContainsKey(plan.childKeysToRemove, BuildUiScanChildKey(UiScanEntryType::File, L"B.txt")), "existing B is replaced before incremental update");
    Check(!ContainsKey(plan.childKeysToRemove, BuildUiScanChildKey(UiScanEntryType::File, L"C.txt")), "unfinished keeps missing C");
}

void UiProjectionProcessesDirectoriesBeforeFiles()
{
    const DirectoryResultDto result = MakeDirectoryResult({
        MakeUiEntry(UiScanEntryType::File, L"alpha.txt"),
        MakeUiEntry(UiScanEntryType::Directory, L"child"),
    });

    const UiScanProjectionPlan plan = BuildUiScanProjectionPlan(result, {});
    Check(plan.directoriesToProject.size() == 1, "directory bucket is populated");
    Check(plan.filesToProject.size() == 1, "file bucket is populated");
    Check(plan.directoriesToProject[0]->name == L"child", "directory is projected through directory pass");
    Check(plan.filesToProject[0]->name == L"alpha.txt", "file is projected through file pass");
}

void UiProjectionRepeatedSnapshotReplacesWithoutExtraRemovals()
{
    const DirectoryResultDto result = MakeDirectoryResult({
        MakeUiEntry(UiScanEntryType::Directory, L"A"),
        MakeUiEntry(UiScanEntryType::File, L"B.txt"),
    });
    const std::vector<std::wstring> existingAfterFirstApply = {
        BuildUiScanChildKey(UiScanEntryType::Directory, L"A"),
        BuildUiScanChildKey(UiScanEntryType::File, L"B.txt"),
    };

    const UiScanProjectionPlan plan = BuildUiScanProjectionPlan(result, existingAfterFirstApply);
    Check(plan.childKeysToRemove.size() == 2, "second apply replaces exactly existing projected children");
    Check(plan.directoriesToProject.size() == 1, "second apply projects one directory");
    Check(plan.filesToProject.size() == 1, "second apply projects one file");
}

void UiProjectionRequestScopeHelperRejectsStaleResults()
{
    const DirectoryResultDto result = MakeDirectoryResult({ MakeUiEntry(UiScanEntryType::File, L"A.txt") });
    Check(IsDirectoryResultForActiveRequest(result, 77), "matching request is active");
    Check(!IsDirectoryResultForActiveRequest(result, 78), "stale request is rejected");
}

void UiProjectionNestedDirectoryRequiresParentKey()
{
    DirectoryResultDto parent = MakeDirectoryResult({ MakeUiEntry(UiScanEntryType::Directory, L"child") });
    const UiScanProjectionPlan parentPlan = BuildUiScanProjectionPlan(parent, {});
    Check(parentPlan.directoriesToProject.size() == 1, "parent DTO creates child directory first");

    DirectoryResultDto child{};
    child.requestId = 77;
    child.path = L"C:\\root\\child";
    child.entries = { MakeUiEntry(UiScanEntryType::File, L"leaf.txt", child.path) };
    child.finished = true;
    Check(child.entries[0].fullPath.starts_with(child.path), "nested child full path belongs to child DTO path");
    const UiScanProjectionPlan childPlan = BuildUiScanProjectionPlan(child, {});
    Check(childPlan.filesToProject.size() == 1, "child DTO is projectable after parent exists");
}

struct TestCase
{
    std::string_view name;
    void (*run)();
};

constexpr TestCase Tests[] = {
    { "multi-step completion", MultiStepScanProgressesToCompletion },
    { "empty completion", EmptyResultCompletesAfterCloseInput },
    { "failure terminal", FailureClearsPendingWorkAndWins },
    { "cancellation", CancellationClearsQueueAndIgnoresLateResults },
    { "external enqueue", ExternalEnqueueIsTrackedBySession },
    { "traversal policy", TraversalPolicyStaysInSession },
    { "reset", ResetRestoresCleanSession },
    { "rpc batch progress roundtrip", RpcDirectoryProgressBatchRoundTrips },
    { "rpc individual progress roundtrip", RpcDirectoryProgressEventStillRoundTrips },
    { "ui projection basic snapshot", UiProjectionPlansBasicDirectorySnapshot },
    { "ui projection finished reconciliation", UiProjectionFinishedReconcilesMissingChildren },
    { "ui projection unfinished reconciliation", UiProjectionUnfinishedKeepsMissingChildren },
    { "ui projection ordering", UiProjectionProcessesDirectoriesBeforeFiles },
    { "ui projection repeated updates", UiProjectionRepeatedSnapshotReplacesWithoutExtraRemovals },
    { "ui projection request scoping", UiProjectionRequestScopeHelperRejectsStaleResults },
    { "ui projection nested parent first", UiProjectionNestedDirectoryRequiresParentKey },
};
}

int main()
{
    try
    {
        for (const TestCase& test : Tests)
            test.run();

        std::cout << "ScanSessionState tests passed: " << std::size(Tests) << '\n';
        return EXIT_SUCCESS;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "ScanSessionState test failed: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
