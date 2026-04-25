namespace WinDirStat.Blazor.Contracts;

public sealed class DelegatingScanObserver(
    Action<DirectoryResultDto> onDirectoryResult,
    Action<ulong>? onCompleted = null,
    Action<ulong, string>? onFailed = null) : IScanObserver
{
    public void OnDirectoryResult(DirectoryResultDto dto) => onDirectoryResult(dto);

    public void OnScanCompleted(ulong requestId) => onCompleted?.Invoke(requestId);

    public void OnScanFailed(ulong requestId, string error) => onFailed?.Invoke(requestId, error);
}
