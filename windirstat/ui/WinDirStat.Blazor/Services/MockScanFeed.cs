using WinDirStat.Blazor.Contracts;

namespace WinDirStat.Blazor.Services;

public sealed class MockScanFeed
{
    public async IAsyncEnumerable<DirectoryResultDto> StreamSampleAsync(ulong requestId)
    {
        await Task.Yield();

        yield return new DirectoryResultDto
        {
            RequestId = requestId,
            Path = @"C:\sample",
            Finished = true,
            Entries =
            [
                new ScanEntryDto { Type = EntryType.Directory, Name = "src", FullPath = @"C:\sample\src" },
                new ScanEntryDto { Type = EntryType.File, Name = "README.md", FullPath = @"C:\sample\README.md", SizeLogical = 1024, SizePhysical = 4096 }
            ]
        };

        yield return new DirectoryResultDto
        {
            RequestId = requestId,
            Path = @"C:\sample\src",
            Finished = true,
            Entries =
            [
                new ScanEntryDto { Type = EntryType.File, Name = "Program.cs", FullPath = @"C:\sample\src\Program.cs", SizeLogical = 2048, SizePhysical = 4096 }
            ]
        };
    }
}
