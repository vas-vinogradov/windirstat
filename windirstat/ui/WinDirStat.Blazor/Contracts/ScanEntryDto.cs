namespace WinDirStat.Blazor.Contracts;

public sealed class ScanEntryDto
{
    public EntryType Type { get; set; }
    public string Name { get; set; } = "";
    public string FullPath { get; set; } = "";
    public ulong SizeLogical { get; set; }
    public ulong SizePhysical { get; set; }
    public ulong FileIndex { get; set; }
    public ulong LastChangeFileTime { get; set; }
    public uint Attributes { get; set; }
    public uint ReparseTag { get; set; }
    public bool IsReserved { get; set; }
    public bool IsOffVolume { get; set; }
    public bool IsProtectedReparsePoint { get; set; }
}
