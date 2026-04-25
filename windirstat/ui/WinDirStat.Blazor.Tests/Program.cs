using WinDirStat.Blazor.Contracts;
using WinDirStat.Blazor.Projection;
using WinDirStat.Blazor.Rpc;
using Microsoft.Extensions.FileProviders;
using Microsoft.Extensions.Hosting;

static void Check(bool condition, string message)
{
    if (!condition)
        throw new InvalidOperationException(message);
}

static void DirectoryProgressEventParses()
{
    const string json = """
        {"kind":"DirectoryProgressEvent","requestId":7,"directoryPath":"C:\\root","files":[{"name":"a.txt","fullPath":"C:\\root\\a.txt","sizePhysical":4096,"sizeLogical":12,"index":44,"lastChange":123,"attributes":32,"reparseTag":0,"isReserved":false}],"directories":[{"name":"child","fullPath":"C:\\root\\child","index":45,"lastChange":124,"attributes":16,"reparseTag":0,"isReserved":false,"isOffVolume":false,"isProtectedReparsePoint":false}],"finished":true}
        """;

    var events = RpcScanEventParser.Parse(json);
    Check(events.Count == 1, "single progress event parses");
    var dto = events[0].DirectoryResult!;
    Check(dto.RequestId == 7, "request id maps");
    Check(dto.Path == @"C:\root", "path maps");
    Check(dto.Entries.Count == 2, "entries map");
    Check(dto.Entries[0].Type == EntryType.Directory, "directories map before files");
    Check(dto.Entries[1].SizeLogical == 12, "file logical size maps");
}

static void DirectoryProgressBatchParses()
{
    const string json = """
        {"kind":"DirectoryProgressBatchEvent","requestId":9,"items":[{"requestId":9,"directoryPath":"C:\\root","files":[],"directories":[{"name":"child","fullPath":"C:\\root\\child","index":1,"lastChange":2,"attributes":16,"reparseTag":0,"isReserved":false,"isOffVolume":false,"isProtectedReparsePoint":false}],"finished":true},{"requestId":9,"directoryPath":"C:\\root\\child","files":[{"name":"leaf.txt","fullPath":"C:\\root\\child\\leaf.txt","sizePhysical":4096,"sizeLogical":50,"index":3,"lastChange":4,"attributes":32,"reparseTag":0,"isReserved":false}],"directories":[],"finished":true}]}
        """;

    var events = RpcScanEventParser.Parse(json);
    Check(events.Count == 2, "batch items replay individually");
    Check(events[0].DirectoryResult!.Entries[0].Name == "child", "first batch item maps");
    Check(events[1].DirectoryResult!.Entries[0].Name == "leaf.txt", "second batch item maps");
}

static void ProjectionReceivesParsedDtos()
{
    var projection = new ScanTreeProjection();
    projection.StartRequest(9);

    foreach (var scanEvent in RpcScanEventParser.Parse("""
        {"kind":"DirectoryProgressEvent","requestId":9,"directoryPath":"C:\\root","files":[{"name":"a.txt","fullPath":"C:\\root\\a.txt","sizePhysical":4096,"sizeLogical":12,"index":44,"lastChange":123,"attributes":32,"reparseTag":0,"isReserved":false}],"directories":[],"finished":true}
        """))
    {
        projection.OnDirectoryResult(scanEvent.DirectoryResult!);
    }

    Check(projection.Root is not null, "projection root is created");
    var root = projection.Root ?? throw new InvalidOperationException("projection root is missing");
    Check(root.Children.Count == 1, "projection receives one file");
    Check(root.SizeLogical == 12, "projection total is sane");
}

static async Task RealScanHostCompletesWhenAvailableAsync()
{
    var contentRoot = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "WinDirStat.Blazor"));
    var repoRoot = Path.GetFullPath(Path.Combine(contentRoot, "..", ".."));
    var hostPath = Path.Combine(repoRoot, "build", "windirstat-scan-host_x64.exe");
    if (!File.Exists(hostPath))
    {
        Console.WriteLine("Real scan-host integration skipped: build\\windirstat-scan-host_x64.exe not found.");
        return;
    }

    var fixture = Path.Combine(Path.GetTempPath(), $"windirstat-blazor-rpc-test-{Environment.ProcessId}-{Guid.NewGuid():N}");
    Directory.CreateDirectory(fixture);
    Directory.CreateDirectory(Path.Combine(fixture, "child"));
    await File.WriteAllTextAsync(Path.Combine(fixture, "alpha.txt"), "alpha");
    await File.WriteAllTextAsync(Path.Combine(fixture, "child", "beta.txt"), "beta");

    var projection = new ScanTreeProjection();
    projection.StartRequest(1);
    var events = 0;
    var entries = 0;
    var observer = new DelegatingScanObserver(
        dto =>
        {
            events++;
            entries += dto.Entries.Count;
            projection.OnDirectoryResult(dto);
        },
        requestId => projection.OnScanCompleted(requestId),
        (requestId, error) => projection.OnScanFailed(requestId, error));

    try
    {
        var client = new NamedPipeScanClient(new TestHostEnvironment(contentRoot));
        await client.StartScanAsync(fixture, observer, 1);
        Check(projection.Completed, "real scan host completed");
        Check(events >= 2, "real scan host emitted directory events");
        Check(entries >= 3, "real scan host emitted expected entries");
        Check(projection.Root?.Children.Count >= 2, "real projection has root children");
    }
    finally
    {
        Directory.Delete(fixture, recursive: true);
    }
}

try
{
    DirectoryProgressEventParses();
    DirectoryProgressBatchParses();
    ProjectionReceivesParsedDtos();
    await RealScanHostCompletesWhenAvailableAsync();
    Console.WriteLine("Blazor RPC tests passed: 4");
    return 0;
}
catch (Exception ex)
{
    Console.Error.WriteLine($"Blazor RPC parser test failed: {ex.Message}");
    return 1;
}

sealed class TestHostEnvironment(string contentRootPath) : IHostEnvironment
{
    public string EnvironmentName { get; set; } = Environments.Development;
    public string ApplicationName { get; set; } = "WinDirStat.Blazor.Tests";
    public string ContentRootPath { get; set; } = contentRootPath;
    public IFileProvider ContentRootFileProvider { get; set; } = new PhysicalFileProvider(contentRootPath);
}
