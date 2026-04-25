param(
    [string]$HostPath = (Join-Path $PSScriptRoot "..\build\windirstat-scan-host_x64.exe"),
    [string]$RootPath = "",
    [string[]]$Modes = @("rust", "legacy"),
    [string]$OutputPath = "",
    [int]$TimeoutSeconds = 120,
    [int]$MaxEvents = 200000,
    [int]$FixtureDepth = 3,
    [int]$FixtureBreadth = 3,
    [int]$FixtureFilesPerDirectory = 3,
    [int]$FixtureFileSizeBytes = 128,
    [uint64]$ProcessorAffinityMask = 0,
    [int]$RustScanThreads = 0,
    [switch]$EnableTiming,
    [switch]$BatchProgress,
    [int]$BatchProgressSize = 64,
    [int]$BatchProgressMaxDelayMs = 10,
    [int]$OutboundQueueCapacity = 0,
    [int]$InboundQueueCapacity = 0,
    [switch]$VerboseHostLogs,
    [switch]$KeepLogs,
    [switch]$KeepFixtures
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Intent: compare scan-level behavior and coarse performance between the
# Rust-owned scan engine and explicit compatibility modes through the existing
# named-pipe RPC adapter.
# Contract: this is a client-side verification tool. It does not participate in
# traversal, discovery, lifecycle decisions, or RPC protocol evolution.

function Resolve-ExistingPath {
    param([string]$Path)

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return $resolved.ProviderPath
}

function Quote-ProcessArgument {
    param([string]$Value)

    return '"' + ($Value -replace '"', '\"') + '"'
}

function Convert-ToUInt64 {
    param($Value)

    if ($null -eq $Value) {
        return [uint64]0
    }

    return [uint64]::Parse(([string]$Value), [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-JsonPropertyValue {
    param(
        $Object,
        [string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }

    return $property.Value
}

function Write-RpcJson {
    param(
        [System.IO.Stream]$Stream,
        $Message
    )

    $json = $Message | ConvertTo-Json -Compress -Depth 20
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $length = [BitConverter]::GetBytes([uint32]$bytes.Length)
    $Stream.Write($length, 0, $length.Length)
    if ($bytes.Length -ne 0) {
        $Stream.Write($bytes, 0, $bytes.Length)
    }

    $Stream.Flush()
}

function Read-ExactBytes {
    param(
        [System.IO.Stream]$Stream,
        [int]$Count
    )

    $buffer = New-Object byte[] $Count
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($read -le 0) {
            throw "RPC pipe closed while reading $Count bytes."
        }

        $offset += $read
    }

    return $buffer
}

function Read-RpcJson {
    param([System.IO.Stream]$Stream)

    $lengthBytes = Read-ExactBytes -Stream $Stream -Count 4
    $length = [BitConverter]::ToUInt32($lengthBytes, 0)
    if ($length -eq 0) {
        return $null
    }

    $jsonBytes = Read-ExactBytes -Stream $Stream -Count ([int]$length)
    $json = [System.Text.Encoding]::UTF8.GetString($jsonBytes)
    return $json | ConvertFrom-Json
}

function New-CompareWorkspace {
    $name = "windirstat-rpc-scan-compare-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N"))
    $path = Join-Path ([System.IO.Path]::GetTempPath()) $name
    New-Item -ItemType Directory -Path $path | Out-Null
    return $path
}

function New-GeneratedFixture {
    param(
        [string]$ParentPath,
        [int]$Depth,
        [int]$Breadth,
        [int]$FilesPerDirectory,
        [int]$FileSizeBytes
    )

    $root = Join-Path $ParentPath "fixture"
    New-Item -ItemType Directory -Path $root | Out-Null
    $payload = "x" * [Math]::Max(1, $FileSizeBytes)

    function Add-FixtureDirectory {
        param(
            [string]$Path,
            [int]$Level
        )

        for ($fileIndex = 0; $fileIndex -lt $FilesPerDirectory; $fileIndex++) {
            $filePath = Join-Path $Path ("file-{0}-{1}.bin" -f $Level, $fileIndex)
            Set-Content -LiteralPath $filePath -Value $payload -NoNewline
        }

        if ($Level -ge $Depth) {
            return
        }

        for ($dirIndex = 0; $dirIndex -lt $Breadth; $dirIndex++) {
            $child = Join-Path $Path ("dir-{0}-{1}" -f $Level, $dirIndex)
            New-Item -ItemType Directory -Path $child | Out-Null
            Add-FixtureDirectory -Path $child -Level ($Level + 1)
        }
    }

    Add-FixtureDirectory -Path $root -Level 1
    return $root
}

function Start-ScanHost {
    param(
        [string]$Mode,
        [string]$HostPath,
        [string]$PipeName,
        [string]$LogFile,
        [bool]$VerboseLogs,
        [uint64]$ProcessorAffinityMask,
        [int]$RustScanThreads,
        [bool]$EnableTiming,
        [bool]$BatchProgress,
        [int]$BatchProgressSize,
        [int]$BatchProgressMaxDelayMs,
        [int]$OutboundQueueCapacity,
        [int]$InboundQueueCapacity
    )

    $signature = Get-AuthenticodeSignature -LiteralPath $HostPath -ErrorAction SilentlyContinue
    $signatureStatus = if ($null -ne $signature) { [string]$signature.Status } else { "Unknown" }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $HostPath
    $psi.Arguments = "--pipe {0} --log-file {1}" -f `
        (Quote-ProcessArgument "\\.\pipe\$PipeName"), `
        (Quote-ProcessArgument $LogFile)
    if ($VerboseLogs) {
        $psi.Arguments += " --log-level verbose"
    }

    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    if ($Mode -eq "default") {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_REMOTE_DISCOVERY_ENGINE")
    }
    else {
        $psi.EnvironmentVariables["WINDIRSTAT_REMOTE_DISCOVERY_ENGINE"] = $Mode
    }

    if ($RustScanThreads -gt 0) {
        $psi.EnvironmentVariables["WINDIRSTAT_RUST_SCAN_THREADS"] = [string]$RustScanThreads
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RUST_SCAN_THREADS")
    }

    if ($EnableTiming) {
        $psi.EnvironmentVariables["WINDIRSTAT_SCAN_TIMING"] = "1"
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_SCAN_TIMING")
    }

    if ($BatchProgress) {
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_BATCH_PROGRESS"] = "1"
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_BATCH_SIZE"] = [string]$BatchProgressSize
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_BATCH_MAX_DELAY_MS"] = [string]$BatchProgressMaxDelayMs
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_BATCH_PROGRESS")
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_BATCH_SIZE")
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_BATCH_MAX_DELAY_MS")
    }

    if ($OutboundQueueCapacity -gt 0) {
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY"] = [string]$OutboundQueueCapacity
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY")
    }

    if ($InboundQueueCapacity -gt 0) {
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY"] = [string]$InboundQueueCapacity
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY")
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    try {
        if (-not $process.Start()) {
            throw "Failed to start scan host '$HostPath'."
        }

        if ($ProcessorAffinityMask -ne 0) {
            $process.ProcessorAffinity = [IntPtr]([int64]$ProcessorAffinityMask)
        }
    }
    catch {
        throw ("Failed to start scan host '{0}' for mode '{1}'. Signature status={2}. On this machine, Smart App Control may block the self-signed dev certificate; the unsigned debug host is the stable local verification path." -f $HostPath, $Mode, $signatureStatus)
    }

    return [pscustomobject]@{
        Process = $process
        SignatureStatus = $signatureStatus
    }
}

function Read-HostTimingSummary {
    param([string]$LogFile)

    if ([string]::IsNullOrWhiteSpace($LogFile) -or -not (Test-Path -LiteralPath $LogFile)) {
        return $null
    }

    $line = Get-Content -LiteralPath $LogFile -ErrorAction SilentlyContinue |
        Where-Object { $_ -match 'event=TimingSummary\s+(\{.*\})' } |
        Select-Object -Last 1
    if ($null -eq $line) {
        return $null
    }

    $match = [regex]::Match([string]$line, 'event=TimingSummary\s+(\{.*\})')
    if (-not $match.Success) {
        return $null
    }

    return ($match.Groups[1].Value | ConvertFrom-Json)
}

function New-TimingBreakdown {
    param(
        [double]$ClientElapsedMs,
        $HostTiming
    )

    if ($null -eq $HostTiming) {
        return $null
    }

    $hostTotalMs = [double]$HostTiming.hostTotalMs
    $nextActionMs = [double]$HostTiming.nextActionMs
    $enqueueWaitMs = [double](Get-JsonPropertyValue -Object $HostTiming -Name "rpcEnqueueWaitMs")
    $rpcMs = [double]$HostTiming.rpcConversionMs + [double]$HostTiming.rpcSerializeMs + [double]$HostTiming.rpcWriteMs + $enqueueWaitMs
    $hostOtherMs = $hostTotalMs - $nextActionMs - $rpcMs
    $clientHostGapMs = $ClientElapsedMs - $hostTotalMs

    function Get-Percent {
        param(
            [double]$Value,
            [double]$Total
        )

        if ($Total -le 0) {
            return 0
        }

        return [Math]::Round(($Value / $Total) * 100.0, 2)
    }

    return [pscustomobject][ordered]@{
        clientElapsedMs = [Math]::Round($ClientElapsedMs, 2)
        hostTotalMs = [Math]::Round($hostTotalMs, 2)
        nextActionSessionMs = [Math]::Round($nextActionMs, 2)
        rpcConversionSerializationWriteMs = [Math]::Round($rpcMs, 2)
        rpcEnqueueWaitMs = [Math]::Round($enqueueWaitMs, 2)
        hostOtherMs = [Math]::Round($hostOtherMs, 2)
        clientHostGapMs = [Math]::Round($clientHostGapMs, 2)
        nextActionHostPercent = Get-Percent -Value $nextActionMs -Total $hostTotalMs
        rpcHostPercent = Get-Percent -Value $rpcMs -Total $hostTotalMs
        hostOtherPercent = Get-Percent -Value $hostOtherMs -Total $hostTotalMs
        clientHostGapClientPercent = Get-Percent -Value $clientHostGapMs -Total $ClientElapsedMs
    }
}

function Add-DirectoryProgressMetrics {
    param(
        $Metrics,
        $Message
    )

    $Metrics.ProgressEventCount++
    $directoryPath = [string](Get-JsonPropertyValue -Object $Message -Name "directoryPath")
    if (-not [string]::IsNullOrEmpty($directoryPath)) {
        $Metrics.VisitedDirectorySet[$directoryPath.ToLowerInvariant()] = $directoryPath
    }

    $files = @(Get-JsonPropertyValue -Object $Message -Name "files")
    $directories = @(Get-JsonPropertyValue -Object $Message -Name "directories")
    $Metrics.FileEntryCount += $files.Count
    $Metrics.DirectoryEntryCount += $directories.Count

    foreach ($file in $files) {
        $Metrics.TotalLogicalSize += Convert-ToUInt64 (Get-JsonPropertyValue -Object $file -Name "sizeLogical")
        $Metrics.TotalPhysicalSize += Convert-ToUInt64 (Get-JsonPropertyValue -Object $file -Name "sizePhysical")
    }
}

function Invoke-ScanMode {
    param(
        [string]$Mode,
        [string]$HostPath,
        [string]$RootPath,
        [string]$WorkspaceRoot,
        [int]$TimeoutSeconds,
        [int]$MaxEvents,
        [bool]$VerboseLogs,
        [uint64]$ProcessorAffinityMask,
        [int]$RustScanThreads,
        [bool]$EnableTiming,
        [bool]$BatchProgress,
        [int]$BatchProgressSize,
        [int]$BatchProgressMaxDelayMs,
        [int]$OutboundQueueCapacity,
        [int]$InboundQueueCapacity
    )

    $pipeName = "WinDirStat.RpcScanCompare.{0}.{1}.{2}" -f $Mode, $PID, ([Guid]::NewGuid().ToString("N"))
    $logFile = Join-Path $WorkspaceRoot ("host-{0}.log" -f $Mode)
    $scanHost = $null
    $stream = $null
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $requestId = [uint64]1
    $terminalKind = ""
    $failureMessages = New-Object System.Collections.Generic.List[string]
    $metrics = [pscustomobject]@{
        ProgressEventCount = 0
        FileEntryCount = 0
        DirectoryEntryCount = 0
        TotalLogicalSize = [uint64]0
        TotalPhysicalSize = [uint64]0
        VisitedDirectorySet = @{}
        PhysicalRpcMessageCount = 0
        BatchMessageCount = 0
        MaxBatchSize = 0
    }

    try {
        $scanHost = Start-ScanHost `
            -Mode $Mode `
            -HostPath $HostPath `
            -PipeName $pipeName `
            -LogFile $logFile `
            -VerboseLogs $VerboseLogs `
            -ProcessorAffinityMask $ProcessorAffinityMask `
            -RustScanThreads $RustScanThreads `
            -EnableTiming $EnableTiming `
            -BatchProgress $BatchProgress `
            -BatchProgressSize $BatchProgressSize `
            -BatchProgressMaxDelayMs $BatchProgressMaxDelayMs `
            -OutboundQueueCapacity $OutboundQueueCapacity `
            -InboundQueueCapacity $InboundQueueCapacity
        $stream = New-Object System.IO.Pipes.NamedPipeClientStream(
            ".",
            $pipeName,
            [System.IO.Pipes.PipeDirection]::InOut,
            [System.IO.Pipes.PipeOptions]::None)
        $stream.Connect($TimeoutSeconds * 1000)
        if ($stream.CanTimeout) {
            $stream.ReadTimeout = $TimeoutSeconds * 1000
            $stream.WriteTimeout = $TimeoutSeconds * 1000
        }

        Write-RpcJson -Stream $stream -Message ([pscustomobject][ordered]@{
            kind = "StartScanRequest"
            requestId = $requestId
            rootPath = $RootPath
            followMountPoints = $false
            followSymbolicLinks = $false
            followJunctions = $false
        })

        Write-RpcJson -Stream $stream -Message ([pscustomobject][ordered]@{
            kind = "CloseRequestInput"
            requestId = $requestId
        })

        while ([string]::IsNullOrEmpty($terminalKind)) {
            if ([DateTime]::UtcNow -gt $deadline) {
                throw "Scan timed out after $TimeoutSeconds seconds for mode '$Mode'."
            }

            if ($metrics.ProgressEventCount -ge $MaxEvents) {
                throw "Scan exceeded MaxEvents=$MaxEvents for mode '$Mode'. Use a smaller root or raise -MaxEvents deliberately."
            }

            $message = Read-RpcJson -Stream $stream
            if ($null -eq $message) {
                continue
            }

            if ([uint64](Get-JsonPropertyValue -Object $message -Name "requestId") -ne $requestId) {
                continue
            }

            $metrics.PhysicalRpcMessageCount++
            switch ([string](Get-JsonPropertyValue -Object $message -Name "kind")) {
                "DirectoryProgressEvent" {
                    Add-DirectoryProgressMetrics -Metrics $metrics -Message $message
                    continue
                }
                "DirectoryProgressBatchEvent" {
                    $items = @(Get-JsonPropertyValue -Object $message -Name "items")
                    $metrics.BatchMessageCount++
                    if ($items.Count -gt $metrics.MaxBatchSize) {
                        $metrics.MaxBatchSize = $items.Count
                    }

                    foreach ($item in $items) {
                        if ([uint64](Get-JsonPropertyValue -Object $item -Name "requestId") -ne $requestId) {
                            continue
                        }

                        Add-DirectoryProgressMetrics -Metrics $metrics -Message $item
                    }
                    continue
                }
                "ScanCompletedEvent" {
                    $terminalKind = "ScanCompletedEvent"
                    continue
                }
                "ScanCanceledEvent" {
                    $terminalKind = "ScanCanceledEvent"
                    continue
                }
                "ScanFailedEvent" {
                    $terminalKind = "ScanFailedEvent"
                    $failurePath = [string](Get-JsonPropertyValue -Object $message -Name "path")
                    $failureMessage = [string](Get-JsonPropertyValue -Object $message -Name "message")
                    if ([string]::IsNullOrEmpty($failureMessage)) {
                        $failureMessage = "Scan failed."
                    }

                    $failureMessages.Add(("{0}: {1}" -f $failurePath, $failureMessage))
                    continue
                }
                default {
                    throw "Unexpected RPC event kind '$((Get-JsonPropertyValue -Object $message -Name "kind"))' for mode '$Mode'."
                }
            }
        }
    }
    catch {
        $terminalKind = if ([string]::IsNullOrEmpty($terminalKind)) { "ToolFailed" } else { $terminalKind }
        $failureMessages.Add([string]$_.Exception.Message)
    }
    finally {
        $timer.Stop()
        if ($null -ne $stream) {
            $stream.Dispose()
        }

        if ($null -ne $scanHost) {
            if (-not $scanHost.Process.WaitForExit(2000)) {
                $scanHost.Process.Kill()
                $scanHost.Process.WaitForExit()
            }

            $scanHost.Process.Dispose()
        }
    }

    $elapsedSeconds = [Math]::Max(0.001, $timer.Elapsed.TotalSeconds)
    $elapsedMs = [Math]::Round($timer.Elapsed.TotalMilliseconds, 2)
    $hostTimingSummary = Read-HostTimingSummary -LogFile $logFile
    $timingBreakdown = New-TimingBreakdown -ClientElapsedMs $elapsedMs -HostTiming $hostTimingSummary
    $childEntryCount = $metrics.FileEntryCount + $metrics.DirectoryEntryCount
    $visitedDirectoryCount = $metrics.VisitedDirectorySet.Count
    $throughputDirsPerSecond = [Math]::Round($visitedDirectoryCount / $elapsedSeconds, 2)
    $throughputEntriesPerSecond = [Math]::Round($childEntryCount / $elapsedSeconds, 2)
    $throughputLogicalBytesPerSecond = [Math]::Round(([double]$metrics.TotalLogicalSize) / $elapsedSeconds, 2)

    return [pscustomobject][ordered]@{
        mode = $Mode
        terminalKind = $terminalKind
        succeeded = ($terminalKind -eq "ScanCompletedEvent")
        failureCount = $failureMessages.Count
        failures = @($failureMessages)
        progressEventCount = $metrics.ProgressEventCount
        physicalRpcMessageCount = $metrics.PhysicalRpcMessageCount
        batchMessageCount = $metrics.BatchMessageCount
        averageBatchSize = if ($metrics.BatchMessageCount -gt 0) { [Math]::Round($metrics.ProgressEventCount / $metrics.BatchMessageCount, 2) } else { 0 }
        maxBatchSize = $metrics.MaxBatchSize
        directoriesVisited = $visitedDirectoryCount
        childEntriesObserved = $childEntryCount
        fileEntriesObserved = $metrics.FileEntryCount
        directoryEntriesObserved = $metrics.DirectoryEntryCount
        totalLogicalSize = $metrics.TotalLogicalSize
        totalPhysicalSize = $metrics.TotalPhysicalSize
        elapsedMs = $elapsedMs
        directoriesPerSecond = $throughputDirsPerSecond
        entriesPerSecond = $throughputEntriesPerSecond
        logicalBytesPerSecond = $throughputLogicalBytesPerSecond
        processorAffinityMask = if ($ProcessorAffinityMask -eq 0) { "default" } else { [string]$ProcessorAffinityMask }
        rustScanThreads = if ($RustScanThreads -gt 0) { [string]$RustScanThreads } else { "default" }
        timingEnabled = $EnableTiming
        batchProgress = $BatchProgress
        batchProgressSize = $BatchProgressSize
        batchProgressMaxDelayMs = $BatchProgressMaxDelayMs
        outboundQueueCapacity = $OutboundQueueCapacity
        inboundQueueCapacity = $InboundQueueCapacity
        hostTiming = $hostTimingSummary
        timingBreakdown = $timingBreakdown
        logFile = $logFile
        signature = if ($null -ne $scanHost) { [string]$scanHost.SignatureStatus } else { "Unknown" }
    }
}

function Compare-ModeSummaries {
    param([object[]]$Results)

    if ($Results.Count -lt 2) {
        return @()
    }

    $baseline = $Results[0]
    $comparisons = New-Object System.Collections.Generic.List[object]
    foreach ($result in @($Results | Select-Object -Skip 1)) {
        $comparisons.Add([pscustomobject][ordered]@{
            baseline = [string]$baseline.mode
            compared = [string]$result.mode
            terminalMatches = ([string]$baseline.terminalKind -eq [string]$result.terminalKind)
            progressEventDelta = ([int64]$result.progressEventCount - [int64]$baseline.progressEventCount)
            directoriesVisitedDelta = ([int64]$result.directoriesVisited - [int64]$baseline.directoriesVisited)
            childEntriesObservedDelta = ([int64]$result.childEntriesObserved - [int64]$baseline.childEntriesObserved)
            totalLogicalSizeDelta = ([int64]$result.totalLogicalSize - [int64]$baseline.totalLogicalSize)
            totalPhysicalSizeDelta = ([int64]$result.totalPhysicalSize - [int64]$baseline.totalPhysicalSize)
            elapsedMsDelta = [Math]::Round(([double]$result.elapsedMs - [double]$baseline.elapsedMs), 2)
        })
    }

    return @($comparisons.ToArray())
}

function Write-ConsoleSummary {
    param(
        [string]$RootPath,
        [object[]]$Results,
        [object[]]$Comparisons,
        [string]$OutputPath
    )

    Write-Host ""
    Write-Host "WinDirStat RPC scan comparison"
    Write-Host ("  root: {0}" -f $RootPath)
    Write-Host ("  output: {0}" -f $OutputPath)
    Write-Host ""

    foreach ($result in $Results) {
        Write-Host ("[{0}] terminal={1} failures={2} events={3} dirs={4} entries={5} logical={6} physical={7} elapsedMs={8} dirs/s={9} entries/s={10}" -f `
            $result.mode,
            $result.terminalKind,
            $result.failureCount,
            $result.progressEventCount,
            $result.directoriesVisited,
            $result.childEntriesObserved,
            $result.totalLogicalSize,
            $result.totalPhysicalSize,
            $result.elapsedMs,
            $result.directoriesPerSecond,
            $result.entriesPerSecond)
        Write-Host ("  rpc: physicalMessages={0} batchMessages={1} avgBatchSize={2} maxBatchSize={3}" -f `
            $result.physicalRpcMessageCount,
            $result.batchMessageCount,
            $result.averageBatchSize,
            $result.maxBatchSize)

        if ($result.failureCount -gt 0) {
            foreach ($failure in @($result.failures | Select-Object -First 3)) {
                Write-Host ("  failure: {0}" -f $failure)
            }

            if ($result.failureCount -gt 3) {
                Write-Host ("  failure: ... {0} more" -f ($result.failureCount - 3))
            }
        }

        if ($null -ne $result.timingBreakdown) {
            Write-Host ("  timing: hostMs={0} nextActionMs={1} rpcMs={2} hostOtherMs={3} clientHostGapMs={4}" -f `
                $result.timingBreakdown.hostTotalMs,
                $result.timingBreakdown.nextActionSessionMs,
                $result.timingBreakdown.rpcConversionSerializationWriteMs,
                $result.timingBreakdown.hostOtherMs,
                $result.timingBreakdown.clientHostGapMs)
            if ($null -ne $result.hostTiming.outboundQueueMaxDepth) {
                Write-Host ("  queue: outboundCapacity={0} maxDepth={1} enqueued={2} written={3} enqueueWaitMs={4}" -f `
                    $result.outboundQueueCapacity,
                    $result.hostTiming.outboundQueueMaxDepth,
                    $result.hostTiming.outboundQueueMessagesEnqueued,
                    $result.hostTiming.outboundQueueMessagesWritten,
                    $result.hostTiming.rpcEnqueueWaitMs)
            }
        }
    }

    if ($Comparisons.Count -gt 0) {
        Write-Host ""
        foreach ($comparison in $Comparisons) {
            Write-Host ("[{0} vs {1}] terminalMatches={2} eventDelta={3} dirDelta={4} entryDelta={5} logicalDelta={6} physicalDelta={7} elapsedMsDelta={8}" -f `
                $comparison.baseline,
                $comparison.compared,
                $comparison.terminalMatches,
                $comparison.progressEventDelta,
                $comparison.directoriesVisitedDelta,
                $comparison.childEntriesObservedDelta,
                $comparison.totalLogicalSizeDelta,
                $comparison.totalPhysicalSizeDelta,
                $comparison.elapsedMsDelta)
        }
    }
}

$resolvedHostPath = Resolve-ExistingPath -Path $HostPath
$workspaceRoot = New-CompareWorkspace
$generatedFixture = [string]::IsNullOrWhiteSpace($RootPath)
$scanRoot = if ($generatedFixture) {
    New-GeneratedFixture `
        -ParentPath $workspaceRoot `
        -Depth $FixtureDepth `
        -Breadth $FixtureBreadth `
        -FilesPerDirectory $FixtureFilesPerDirectory `
        -FileSizeBytes $FixtureFileSizeBytes
}
else {
    Resolve-ExistingPath -Path $RootPath
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("windirstat-rpc-scan-compare-{0}-{1}.json" -f $PID, ([Guid]::NewGuid().ToString("N")))
}
else {
    $outputParent = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($outputParent) -and -not (Test-Path -LiteralPath $outputParent)) {
        New-Item -ItemType Directory -Path $outputParent | Out-Null
    }
}

$results = New-Object System.Collections.Generic.List[object]
foreach ($mode in $Modes) {
    $normalizedMode = ([string]$mode).ToLowerInvariant()
    if (@("default", "rust", "legacy", "basic-replacement", "stub") -notcontains $normalizedMode) {
        throw "Unsupported mode '$mode'. Expected default, rust, legacy, basic-replacement, or stub."
    }

    $results.Add((Invoke-ScanMode `
        -Mode $normalizedMode `
        -HostPath $resolvedHostPath `
        -RootPath $scanRoot `
        -WorkspaceRoot $workspaceRoot `
        -TimeoutSeconds $TimeoutSeconds `
        -MaxEvents $MaxEvents `
        -VerboseLogs ([bool]$VerboseHostLogs) `
        -ProcessorAffinityMask $ProcessorAffinityMask `
        -RustScanThreads $RustScanThreads `
        -EnableTiming ([bool]$EnableTiming) `
        -BatchProgress ([bool]$BatchProgress) `
        -BatchProgressSize $BatchProgressSize `
        -BatchProgressMaxDelayMs $BatchProgressMaxDelayMs `
        -OutboundQueueCapacity $OutboundQueueCapacity `
        -InboundQueueCapacity $InboundQueueCapacity))
}

$resultArray = @($results.ToArray())
$comparisons = @(Compare-ModeSummaries -Results $resultArray)
$hasFailures = @($resultArray | Where-Object { -not $_.succeeded }).Count -gt 0
$report = [pscustomobject][ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    hostPath = $resolvedHostPath
    rootPath = $scanRoot
    generatedFixture = $generatedFixture
    timeoutSeconds = $TimeoutSeconds
    maxEvents = $MaxEvents
    processorAffinityMask = if ($ProcessorAffinityMask -eq 0) { "default" } else { [string]$ProcessorAffinityMask }
    rustScanThreads = if ($RustScanThreads -gt 0) { [string]$RustScanThreads } else { "default" }
    timingEnabled = [bool]$EnableTiming
    batchProgress = [bool]$BatchProgress
    batchProgressSize = $BatchProgressSize
    batchProgressMaxDelayMs = $BatchProgressMaxDelayMs
    outboundQueueCapacity = $OutboundQueueCapacity
    inboundQueueCapacity = $InboundQueueCapacity
    modes = @($Modes)
    hasFailures = $hasFailures
    results = $resultArray
    comparisons = $comparisons
}

$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-ConsoleSummary -RootPath $scanRoot -Results $resultArray -Comparisons $comparisons -OutputPath $OutputPath

if ($hasFailures) {
    Write-Warning "One or more scan modes failed. Host logs were preserved in $workspaceRoot."
    exit 1
}

if ($KeepLogs -or $KeepFixtures) {
    Write-Host ("Preserved workspace: {0}" -f $workspaceRoot)
}
else {
    Remove-Item -LiteralPath $workspaceRoot -Recurse -Force
}
