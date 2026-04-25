using System.Text.Json.Serialization;

namespace WinDirStat.Blazor.Rpc;

internal sealed class RpcKindDto
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = "";
}

internal sealed class RpcStartScanRequestDto
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = "StartScanRequest";

    [JsonPropertyName("requestId")]
    public ulong RequestId { get; set; }

    [JsonPropertyName("rootPath")]
    public string RootPath { get; set; } = "";

    [JsonPropertyName("followMountPoints")]
    public bool FollowMountPoints { get; set; }

    [JsonPropertyName("followSymbolicLinks")]
    public bool FollowSymbolicLinks { get; set; }

    [JsonPropertyName("followJunctions")]
    public bool FollowJunctions { get; set; }
}

internal sealed class RpcCloseRequestInputDto
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = "CloseRequestInput";

    [JsonPropertyName("requestId")]
    public ulong RequestId { get; set; }
}

internal sealed class RpcDirectoryProgressEventDto
{
    [JsonPropertyName("kind")]
    public string? Kind { get; set; }

    [JsonPropertyName("requestId")]
    public ulong RequestId { get; set; }

    [JsonPropertyName("directoryPath")]
    public string DirectoryPath { get; set; } = "";

    [JsonPropertyName("files")]
    public List<RpcDiscoveredFileDto> Files { get; set; } = [];

    [JsonPropertyName("directories")]
    public List<RpcDiscoveredDirectoryDto> Directories { get; set; } = [];

    [JsonPropertyName("finished")]
    public bool Finished { get; set; }
}

internal sealed class RpcDirectoryProgressBatchEventDto
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = "DirectoryProgressBatchEvent";

    [JsonPropertyName("requestId")]
    public ulong RequestId { get; set; }

    [JsonPropertyName("items")]
    public List<RpcDirectoryProgressEventDto> Items { get; set; } = [];
}

internal sealed class RpcDiscoveredFileDto
{
    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("fullPath")]
    public string FullPath { get; set; } = "";

    [JsonPropertyName("sizePhysical")]
    public ulong SizePhysical { get; set; }

    [JsonPropertyName("sizeLogical")]
    public ulong SizeLogical { get; set; }

    [JsonPropertyName("index")]
    public ulong Index { get; set; }

    [JsonPropertyName("lastChange")]
    public ulong LastChange { get; set; }

    [JsonPropertyName("attributes")]
    public uint Attributes { get; set; }

    [JsonPropertyName("reparseTag")]
    public uint ReparseTag { get; set; }

    [JsonPropertyName("isReserved")]
    public bool IsReserved { get; set; }
}

internal sealed class RpcDiscoveredDirectoryDto
{
    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("fullPath")]
    public string FullPath { get; set; } = "";

    [JsonPropertyName("index")]
    public ulong Index { get; set; }

    [JsonPropertyName("lastChange")]
    public ulong LastChange { get; set; }

    [JsonPropertyName("attributes")]
    public uint Attributes { get; set; }

    [JsonPropertyName("reparseTag")]
    public uint ReparseTag { get; set; }

    [JsonPropertyName("isReserved")]
    public bool IsReserved { get; set; }

    [JsonPropertyName("isOffVolume")]
    public bool IsOffVolume { get; set; }

    [JsonPropertyName("isProtectedReparsePoint")]
    public bool IsProtectedReparsePoint { get; set; }
}

internal sealed class RpcTerminalEventDto
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = "";

    [JsonPropertyName("requestId")]
    public ulong RequestId { get; set; }

    [JsonPropertyName("reason")]
    public uint Reason { get; set; }

    [JsonPropertyName("path")]
    public string Path { get; set; } = "";

    [JsonPropertyName("message")]
    public string Message { get; set; } = "";

    [JsonPropertyName("errorCode")]
    public uint ErrorCode { get; set; }
}
