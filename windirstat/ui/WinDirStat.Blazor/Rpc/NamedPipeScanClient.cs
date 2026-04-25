using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using WinDirStat.Blazor.Contracts;

namespace WinDirStat.Blazor.Rpc;

public sealed class NamedPipeScanClient(IHostEnvironment environment)
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = null
    };

    public async Task StartScanAsync(
        string rootPath,
        IScanObserver observer,
        ulong requestId = 1,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(rootPath))
            throw new ArgumentException("Root path is required.", nameof(rootPath));

        var pipeName = $"WinDirStat.Blazor.{Guid.NewGuid():N}";
        var fullPipeName = $@"\\.\pipe\{pipeName}";
        using var host = StartScanHost(fullPipeName);

        await using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        await pipe.ConnectAsync(TimeSpan.FromSeconds(5), cancellationToken);

        await WriteJsonAsync(pipe, new RpcStartScanRequestDto
        {
            RequestId = requestId,
            RootPath = rootPath,
            FollowMountPoints = true,
            FollowSymbolicLinks = true,
            FollowJunctions = true
        }, cancellationToken);

        await WriteJsonAsync(pipe, new RpcCloseRequestInputDto { RequestId = requestId }, cancellationToken);

        while (!cancellationToken.IsCancellationRequested)
        {
            var json = await ReadJsonAsync(pipe, cancellationToken);
            foreach (var scanEvent in RpcScanEventParser.Parse(json))
            {
                switch (scanEvent.Kind)
                {
                case RpcScanEventKind.DirectoryResult:
                    observer.OnDirectoryResult(scanEvent.DirectoryResult!);
                    break;
                case RpcScanEventKind.Completed:
                    observer.OnScanCompleted(scanEvent.RequestId);
                    return;
                case RpcScanEventKind.Failed:
                case RpcScanEventKind.Canceled:
                    observer.OnScanFailed(scanEvent.RequestId, scanEvent.Error);
                    return;
                }
            }
        }
    }

    private Process StartScanHost(string pipeName)
    {
        var hostPath = ResolveScanHostPath();
        var logPath = Path.Combine(Path.GetTempPath(), $"windirstat-blazor-scan-host-{Environment.ProcessId}-{Guid.NewGuid():N}.log");
        var startInfo = new ProcessStartInfo
        {
            FileName = hostPath,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        startInfo.ArgumentList.Add("--pipe");
        startInfo.ArgumentList.Add(pipeName);
        startInfo.ArgumentList.Add("--log-file");
        startInfo.ArgumentList.Add(logPath);

        return Process.Start(startInfo)
            ?? throw new InvalidOperationException($"Failed to start scan host at '{hostPath}'.");
    }

    private string ResolveScanHostPath()
    {
        var current = new DirectoryInfo(environment.ContentRootPath);
        while (current is not null)
        {
            var candidate = Path.Combine(current.FullName, "build", "windirstat-scan-host_x64.exe");
            if (File.Exists(candidate))
                return candidate;

            current = current.Parent;
        }

        throw new FileNotFoundException("Could not locate build\\windirstat-scan-host_x64.exe from the Blazor content root.");
    }

    private static async Task WriteJsonAsync<T>(Stream stream, T message, CancellationToken cancellationToken)
    {
        var json = JsonSerializer.Serialize(message, JsonOptions);
        var payload = Encoding.UTF8.GetBytes(json);
        var length = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(length, checked((uint)payload.Length));
        await stream.WriteAsync(length, cancellationToken);
        await stream.WriteAsync(payload, cancellationToken);
        await stream.FlushAsync(cancellationToken);
    }

    private static async Task<string> ReadJsonAsync(Stream stream, CancellationToken cancellationToken)
    {
        var lengthBuffer = new byte[4];
        await ReadExactlyAsync(stream, lengthBuffer, cancellationToken);
        var length = BinaryPrimitives.ReadUInt32LittleEndian(lengthBuffer);
        var payload = new byte[length];
        await ReadExactlyAsync(stream, payload, cancellationToken);
        return Encoding.UTF8.GetString(payload);
    }

    private static async Task ReadExactlyAsync(Stream stream, byte[] buffer, CancellationToken cancellationToken)
    {
        var offset = 0;
        while (offset < buffer.Length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(offset, buffer.Length - offset), cancellationToken);
            if (read == 0)
                throw new EndOfStreamException("RPC pipe closed while reading a frame.");

            offset += read;
        }
    }
}
