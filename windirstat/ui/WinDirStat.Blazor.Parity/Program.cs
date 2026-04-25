using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Microsoft.Extensions.FileProviders;
using Microsoft.Extensions.Hosting;
using WinDirStat.Blazor.Contracts;
using WinDirStat.Blazor.Rpc;

namespace WinDirStat.Blazor.Parity;

internal static class Program
{
    // Intent: sidecar validation only. This tool compares the production MFC
    // quiet CSV path with the prototype C# named-pipe client without changing
    // scan ownership, RPC protocol, or UI behavior.
    private const uint ItemDrive = 1u << 1;
    private const uint ItemDirectory = 1u << 2;
    private const uint ItemFile = 1u << 3;

    private static async Task<int> Main(string[] args)
    {
        var options = ParityOptions.Parse(args);
        if (options is null)
        {
            PrintUsage();
            return 2;
        }

        var rootPath = Path.GetFullPath(options.RootPath);
        if (!Directory.Exists(rootPath))
            throw new DirectoryNotFoundException($"Root path does not exist: {rootPath}");

        var toolRoot = FindToolRoot(Directory.GetCurrentDirectory())
            ?? FindToolRoot(AppContext.BaseDirectory)
            ?? throw new DirectoryNotFoundException("Could not locate WinDirStat workspace root from current directory.");
        var appPath = options.AppPath is not null
            ? Path.GetFullPath(options.AppPath)
            : Path.Combine(toolRoot, "build", "WinDirStat_x64.exe");
        if (!File.Exists(appPath))
            throw new FileNotFoundException("Could not locate WinDirStat_x64.exe.", appPath);

        var outputPath = Path.GetFullPath(options.OutputPath
            ?? Path.Combine(Path.GetTempPath(), $"windirstat-blazor-mfc-parity-{DateTimeOffset.UtcNow:yyyyMMdd-HHmmss}.json"));
        Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);

        var workspace = ParityWorkspace.Create();
        Console.WriteLine($"Root: {rootPath}");
        Console.WriteLine($"MFC app: {appPath}");

        var mfc = await MfcCsvRunner.RunAsync(appPath, rootPath, workspace.Path, options.TimeoutSeconds);
        var blazor = await BlazorPipeRunner.RunAsync(toolRoot, rootPath, options.TimeoutSeconds);
        var comparison = ParityComparison.Create(mfc, blazor);

        var report = new ParityReport(
            DateTimeOffset.UtcNow,
            rootPath,
            workspace.Path,
            mfc,
            blazor,
            comparison);

        await File.WriteAllTextAsync(outputPath, JsonSerializer.Serialize(report, JsonOptions));

        PrintSummary(report, outputPath);
        return comparison.Matches ? 0 : 1;
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true
    };

    private static void PrintUsage()
    {
        Console.WriteLine("Usage:");
        Console.WriteLine("  dotnet run --project ui/WinDirStat.Blazor.Parity -- <root-path> [--output report.json] [--app build/WinDirStat_x64.exe] [--timeout 600]");
    }

    private static void PrintSummary(ParityReport report, string outputPath)
    {
        Console.WriteLine();
        Console.WriteLine("Blazor/MFC parity summary");
        Console.WriteLine($"  MFC:    {FormatMetrics(report.Mfc)}");
        Console.WriteLine($"  Blazor: {FormatMetrics(report.Blazor)}");
        Console.WriteLine($"  Delta:  directories={report.Comparison.DirectoryCountDelta}, files={report.Comparison.FileCountDelta}, entries={report.Comparison.TotalEntriesDelta}, logical={report.Comparison.LogicalSizeDelta}, physical={report.Comparison.PhysicalSizeDelta}");
        Console.WriteLine($"  Terminal outcome match: {report.Comparison.TerminalOutcomeMatches}");
        Console.WriteLine($"  Result: {(report.Comparison.Matches ? "PASS" : "FAIL")}");
        Console.WriteLine($"  JSON: {outputPath}");
        Console.WriteLine($"  Workspace: {report.WorkspacePath}");
    }

    private static string FormatMetrics(ScanMetrics metrics) =>
        $"outcome={metrics.TerminalOutcome}, directories={metrics.DirectoryCount}, files={metrics.FileCount}, entries={metrics.TotalEntries}, logical={metrics.LogicalSize}, physical={metrics.PhysicalSize}, elapsedMs={metrics.ElapsedMilliseconds}";

    private static string? FindToolRoot(string startPath)
    {
        var current = new DirectoryInfo(startPath);
        while (current is not null)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "ui", "WinDirStat.Blazor"))
                && File.Exists(Path.Combine(current.FullName, "build", "WinDirStat_x64.exe"))
                && File.Exists(Path.Combine(current.FullName, "build", "windirstat-scan-host_x64.exe")))
            {
                return current.FullName;
            }

            current = current.Parent;
        }

        return null;
    }

    private sealed record ParityOptions(
        string RootPath,
        string? OutputPath,
        string? AppPath,
        int TimeoutSeconds)
    {
        public static ParityOptions? Parse(string[] args)
        {
            string? rootPath = null;
            string? outputPath = null;
            string? appPath = null;
            var timeoutSeconds = 600;

            for (var i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                case "--output" when i + 1 < args.Length:
                    outputPath = args[++i];
                    break;
                case "--app" when i + 1 < args.Length:
                    appPath = args[++i];
                    break;
                case "--timeout" when i + 1 < args.Length && int.TryParse(args[++i], NumberStyles.Integer, CultureInfo.InvariantCulture, out var timeout):
                    timeoutSeconds = timeout;
                    break;
                default:
                    if (rootPath is null && !args[i].StartsWith("--", StringComparison.Ordinal))
                        rootPath = args[i];
                    else
                        return null;
                    break;
                }
            }

            return rootPath is null ? null : new ParityOptions(rootPath, outputPath, appPath, timeoutSeconds);
        }
    }

    private static class MfcCsvRunner
    {
        // Contract: this runner mirrors tools/app_rpc_scan_perf.ps1 closely
        // enough for parity validation, with experimental queue/batch paths
        // explicitly absent from the child process environment.
        public static async Task<ScanMetrics> RunAsync(string appPath, string rootPath, string workspacePath, int timeoutSeconds)
        {
            var csvPath = Path.Combine(workspacePath, "mfc-scan.csv");
            var process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = appPath,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    WorkingDirectory = Path.GetDirectoryName(appPath)!
                }
            };
            process.StartInfo.ArgumentList.Add(rootPath);
            process.StartInfo.ArgumentList.Add("/savetocsv");
            process.StartInfo.ArgumentList.Add(csvPath);
            process.StartInfo.ArgumentList.Add("--scan-engine");
            process.StartInfo.ArgumentList.Add("rpc");
            process.StartInfo.Environment["WINDIRSTAT_SCAN_ENGINE"] = "rpc";
            process.StartInfo.Environment["WINDIRSTAT_RUST_SCAN_THREADS"] = "8";
            process.StartInfo.Environment["WINDIRSTAT_DISABLE_ELEVATION"] = "1";
            process.StartInfo.Environment.Remove("WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY");
            process.StartInfo.Environment.Remove("WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY");
            process.StartInfo.Environment.Remove("WINDIRSTAT_RPC_BATCH_PROGRESS");

            var timer = Stopwatch.StartNew();
            process.Start();
            var exited = await WaitForExitAsync(process, TimeSpan.FromSeconds(timeoutSeconds));
            if (!exited)
            {
                process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync();
            }
            timer.Stop();

            var stdout = await process.StandardOutput.ReadToEndAsync();
            var stderr = await process.StandardError.ReadToEndAsync();
            await File.WriteAllTextAsync(Path.Combine(workspacePath, "mfc-stdout.txt"), stdout);
            await File.WriteAllTextAsync(Path.Combine(workspacePath, "mfc-stderr.txt"), stderr);

            if (!exited)
            {
                return ScanMetrics.Failed("TimedOut", timer.ElapsedMilliseconds, csvPath, process.ExitCode);
            }

            if (process.ExitCode != 0 || !File.Exists(csvPath))
            {
                return ScanMetrics.Failed("Failed", timer.ElapsedMilliseconds, csvPath, process.ExitCode);
            }

            var parsed = CsvMetricsParser.Parse(csvPath);
            return parsed with
            {
                Source = "MFC quiet CSV",
                TerminalOutcome = "Completed",
                ElapsedMilliseconds = timer.ElapsedMilliseconds,
                ArtifactPath = csvPath,
                ExitCode = process.ExitCode
            };
        }
    }

    private static class BlazorPipeRunner
    {
        public static async Task<ScanMetrics> RunAsync(string repoRoot, string rootPath, int timeoutSeconds)
        {
            var observer = new MetricsObserver();
            var environment = new ToolHostEnvironment(repoRoot);
            var client = new NamedPipeScanClient(environment);

            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(timeoutSeconds));
            var timer = Stopwatch.StartNew();
            try
            {
                await client.StartScanAsync(rootPath, observer, requestId: 1, timeout.Token);
                timer.Stop();
                return observer.ToMetrics(timer.ElapsedMilliseconds);
            }
            catch (OperationCanceledException)
            {
                timer.Stop();
                return observer.ToMetrics(timer.ElapsedMilliseconds) with { TerminalOutcome = "TimedOut" };
            }
            catch (Exception ex)
            {
                timer.Stop();
                return observer.ToMetrics(timer.ElapsedMilliseconds) with { TerminalOutcome = "Failed", Error = ex.Message };
            }
        }
    }

    private sealed class MetricsObserver : IScanObserver
    {
        // Assumption: parity compares the scan snapshot surfaced to UI clients:
        // distinct directories plus file entries, with file sizes accumulated
        // once from immediate-child DTO entries.
        private readonly HashSet<string> directories = new(StringComparer.OrdinalIgnoreCase);
        private ulong fileCount;
        private ulong logicalSize;
        private ulong physicalSize;
        private string terminalOutcome = "Unknown";
        private string? error;

        public ulong EventCount { get; private set; }

        public void OnDirectoryResult(DirectoryResultDto dto)
        {
            EventCount++;
            if (!string.IsNullOrWhiteSpace(dto.Path))
                directories.Add(Path.GetFullPath(dto.Path));

            foreach (var entry in dto.Entries)
            {
                if (entry.Type == EntryType.Directory)
                {
                    if (!string.IsNullOrWhiteSpace(entry.FullPath))
                        directories.Add(Path.GetFullPath(entry.FullPath));
                    continue;
                }

                if (entry.Type != EntryType.File)
                    continue;

                fileCount++;
                logicalSize += entry.SizeLogical;
                physicalSize += entry.SizePhysical;
            }
        }

        public void OnScanCompleted(ulong requestId)
        {
            terminalOutcome = "Completed";
        }

        public void OnScanFailed(ulong requestId, string error)
        {
            terminalOutcome = error.StartsWith("Canceled", StringComparison.OrdinalIgnoreCase) ? "Canceled" : "Failed";
            this.error = error;
        }

        public ScanMetrics ToMetrics(long elapsedMilliseconds)
        {
            var directoryCount = (ulong)directories.Count;
            return new ScanMetrics(
                "Blazor named-pipe client",
                terminalOutcome,
                elapsedMilliseconds,
                directoryCount,
                fileCount,
                directoryCount + fileCount,
                logicalSize,
                physicalSize,
                EventCount,
                null,
                null,
                error);
        }
    }

    private static class CsvMetricsParser
    {
        public static ScanMetrics Parse(string csvPath)
        {
            using var reader = new StreamReader(csvPath);
            _ = reader.ReadLine();
            var firstDataLine = reader.ReadLine() ?? throw new InvalidDataException("MFC CSV did not contain a root row.");
            var firstFields = SplitCsv(firstDataLine);
            var logicalSize = ParseUlong(firstFields[3]);
            var physicalSize = ParseUlong(firstFields[4]);

            ulong directoryCount = 0;
            ulong fileCount = 0;
            CountRow(firstFields, ref directoryCount, ref fileCount);

            while (reader.ReadLine() is { } line)
            {
                if (string.IsNullOrWhiteSpace(line))
                    continue;

                CountRow(SplitCsv(line), ref directoryCount, ref fileCount);
            }

            return new ScanMetrics(
                "",
                "",
                0,
                directoryCount,
                fileCount,
                directoryCount + fileCount,
                logicalSize,
                physicalSize,
                0,
                csvPath,
                null,
                null);
        }

        private static void CountRow(IReadOnlyList<string> fields, ref ulong directoryCount, ref ulong fileCount)
        {
            if (fields.Count < 8)
                throw new InvalidDataException("MFC CSV row does not contain the WinDirStat item attributes column.");

            var itemType = ParseHexUInt(fields[7]);
            if ((itemType & ItemFile) != 0)
                fileCount++;
            else if ((itemType & (ItemDrive | ItemDirectory)) != 0)
                directoryCount++;
        }

        private static List<string> SplitCsv(string line)
        {
            var values = new List<string>();
            var index = 0;
            while (index <= line.Length)
            {
                if (index == line.Length)
                {
                    values.Add("");
                    break;
                }

                if (line[index] == '"')
                {
                    index++;
                    var start = index;
                    while (index < line.Length && line[index] != '"')
                        index++;
                    values.Add(line[start..index]);
                    index++;
                }
                else
                {
                    var start = index;
                    while (index < line.Length && line[index] != ',')
                        index++;
                    values.Add(line[start..index]);
                }

                if (index < line.Length && line[index] == ',')
                    index++;
                else if (index >= line.Length)
                    break;
            }

            return values;
        }

        private static ulong ParseUlong(string value) =>
            ulong.Parse(value, NumberStyles.Integer, CultureInfo.InvariantCulture);

        private static uint ParseHexUInt(string value)
        {
            var text = value.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? value[2..] : value;
            return uint.Parse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        }
    }

    private sealed class ToolHostEnvironment(string contentRootPath) : IHostEnvironment
    {
        public string EnvironmentName { get; set; } = Environments.Production;
        public string ApplicationName { get; set; } = "WinDirStat.Blazor.Parity";
        public string ContentRootPath { get; set; } = contentRootPath;
        public IFileProvider ContentRootFileProvider { get; set; } = new PhysicalFileProvider(contentRootPath);
    }

    private sealed class ParityWorkspace
    {
        private ParityWorkspace(string path)
        {
            Path = path;
        }

        public string Path { get; }

        public static ParityWorkspace Create()
        {
            var path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"windirstat-blazor-mfc-parity-{Environment.ProcessId}-{Guid.NewGuid():N}");
            Directory.CreateDirectory(path);
            return new ParityWorkspace(path);
        }

    }

    private static async Task<bool> WaitForExitAsync(Process process, TimeSpan timeout)
    {
        using var timeoutSource = new CancellationTokenSource(timeout);
        try
        {
            await process.WaitForExitAsync(timeoutSource.Token);
            return true;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
    }
}

public sealed record ScanMetrics(
    string Source,
    string TerminalOutcome,
    long ElapsedMilliseconds,
    ulong DirectoryCount,
    ulong FileCount,
    ulong TotalEntries,
    ulong LogicalSize,
    ulong PhysicalSize,
    ulong EventCount,
    string? ArtifactPath,
    int? ExitCode,
    string? Error)
{
    public static ScanMetrics Failed(string outcome, long elapsedMilliseconds, string? artifactPath, int? exitCode) =>
        new("MFC quiet CSV", outcome, elapsedMilliseconds, 0, 0, 0, 0, 0, 0, artifactPath, exitCode, null);
}

public sealed record ParityComparison(
    long DirectoryCountDelta,
    long FileCountDelta,
    long TotalEntriesDelta,
    long LogicalSizeDelta,
    long PhysicalSizeDelta,
    bool TerminalOutcomeMatches,
    bool Matches)
{
    public static ParityComparison Create(ScanMetrics mfc, ScanMetrics blazor)
    {
        var terminalMatches = string.Equals(mfc.TerminalOutcome, blazor.TerminalOutcome, StringComparison.Ordinal);
        var directoryDelta = Delta(mfc.DirectoryCount, blazor.DirectoryCount);
        var fileDelta = Delta(mfc.FileCount, blazor.FileCount);
        var entriesDelta = Delta(mfc.TotalEntries, blazor.TotalEntries);
        var logicalDelta = Delta(mfc.LogicalSize, blazor.LogicalSize);
        var physicalDelta = Delta(mfc.PhysicalSize, blazor.PhysicalSize);
        var matches = terminalMatches
            && directoryDelta == 0
            && fileDelta == 0
            && entriesDelta == 0
            && logicalDelta == 0
            && physicalDelta == 0;

        return new ParityComparison(directoryDelta, fileDelta, entriesDelta, logicalDelta, physicalDelta, terminalMatches, matches);
    }

    private static long Delta(ulong left, ulong right) =>
        checked((long)right - (long)left);
}

public sealed record ParityReport(
    DateTimeOffset GeneratedAtUtc,
    string RootPath,
    string WorkspacePath,
    ScanMetrics Mfc,
    ScanMetrics Blazor,
    ParityComparison Comparison);
