namespace WinDirStat.Blazor.Contracts;

public interface IScanObserver
{
    void OnDirectoryResult(DirectoryResultDto dto);
    void OnScanCompleted(ulong requestId);
    void OnScanFailed(ulong requestId, string error);
}
