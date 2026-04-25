using System.Text.Json;
using WinDirStat.Blazor.Contracts;

namespace WinDirStat.Blazor.Rpc;

public enum RpcScanEventKind
{
    DirectoryResult,
    Completed,
    Failed,
    Canceled
}

public sealed record RpcScanEvent(
    RpcScanEventKind Kind,
    ulong RequestId,
    DirectoryResultDto? DirectoryResult = null,
    string Error = "");

public static class RpcScanEventParser
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = false
    };

    public static IReadOnlyList<RpcScanEvent> Parse(string json)
    {
        var kind = JsonSerializer.Deserialize<RpcKindDto>(json, JsonOptions)?.Kind
            ?? throw new InvalidOperationException("RPC event has no kind.");

        return kind switch
        {
            "DirectoryProgressEvent" => [ParseDirectoryProgress(json)],
            "DirectoryProgressBatchEvent" => ParseDirectoryProgressBatch(json),
            "ScanCompletedEvent" => [ParseTerminal(json, RpcScanEventKind.Completed)],
            "ScanFailedEvent" => [ParseTerminal(json, RpcScanEventKind.Failed)],
            "ScanCanceledEvent" => [ParseTerminal(json, RpcScanEventKind.Canceled)],
            _ => throw new InvalidOperationException($"Unsupported RPC event kind '{kind}'.")
        };
    }

    private static RpcScanEvent ParseDirectoryProgress(string json)
    {
        var dto = JsonSerializer.Deserialize<RpcDirectoryProgressEventDto>(json, JsonOptions)
            ?? throw new InvalidOperationException("Invalid DirectoryProgressEvent.");
        var result = MapDirectoryResult(dto);
        return new RpcScanEvent(RpcScanEventKind.DirectoryResult, result.RequestId, result);
    }

    private static IReadOnlyList<RpcScanEvent> ParseDirectoryProgressBatch(string json)
    {
        var batch = JsonSerializer.Deserialize<RpcDirectoryProgressBatchEventDto>(json, JsonOptions)
            ?? throw new InvalidOperationException("Invalid DirectoryProgressBatchEvent.");

        return batch.Items
            .Select(item =>
            {
                if (item.RequestId == 0)
                    item.RequestId = batch.RequestId;

                var result = MapDirectoryResult(item);
                return new RpcScanEvent(RpcScanEventKind.DirectoryResult, result.RequestId, result);
            })
            .ToArray();
    }

    private static RpcScanEvent ParseTerminal(string json, RpcScanEventKind kind)
    {
        var terminal = JsonSerializer.Deserialize<RpcTerminalEventDto>(json, JsonOptions)
            ?? throw new InvalidOperationException("Invalid terminal event.");

        var error = kind switch
        {
            RpcScanEventKind.Failed => $"{terminal.Message} ({terminal.ErrorCode}) path={terminal.Path}",
            RpcScanEventKind.Canceled => $"Canceled reason={terminal.Reason}",
            _ => ""
        };

        return new RpcScanEvent(kind, terminal.RequestId, Error: error);
    }

    private static DirectoryResultDto MapDirectoryResult(RpcDirectoryProgressEventDto progress)
    {
        var result = new DirectoryResultDto
        {
            RequestId = progress.RequestId,
            Path = progress.DirectoryPath,
            Finished = progress.Finished
        };

        result.Entries.AddRange(progress.Directories.Select(directory => new ScanEntryDto
        {
            Type = EntryType.Directory,
            Name = directory.Name,
            FullPath = directory.FullPath,
            FileIndex = directory.Index,
            LastChangeFileTime = directory.LastChange,
            Attributes = directory.Attributes,
            ReparseTag = directory.ReparseTag,
            IsReserved = directory.IsReserved,
            IsOffVolume = directory.IsOffVolume,
            IsProtectedReparsePoint = directory.IsProtectedReparsePoint
        }));

        result.Entries.AddRange(progress.Files.Select(file => new ScanEntryDto
        {
            Type = EntryType.File,
            Name = file.Name,
            FullPath = file.FullPath,
            SizeLogical = file.SizeLogical,
            SizePhysical = file.SizePhysical,
            FileIndex = file.Index,
            LastChangeFileTime = file.LastChange,
            Attributes = file.Attributes,
            ReparseTag = file.ReparseTag,
            IsReserved = file.IsReserved
        }));

        return result;
    }
}
