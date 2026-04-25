namespace WinDirStat.Blazor.Contracts;

public sealed class DirectoryResultDto
{
    public ulong RequestId { get; set; }
    public string Path { get; set; } = "";
    public List<ScanEntryDto> Entries { get; set; } = [];
    public bool Finished { get; set; }
}
