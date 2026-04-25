namespace WinDirStat.Blazor.Projection;

public sealed class ScanNode
{
    public required string Name { get; init; }
    public required string Path { get; init; }
    public bool IsDirectory { get; init; }
    public ulong SizeLogical { get; set; }
    public ulong SizePhysical { get; set; }
    public ulong FileIndex { get; set; }
    public Dictionary<string, ScanNode> Children { get; } = new(StringComparer.OrdinalIgnoreCase);
}
