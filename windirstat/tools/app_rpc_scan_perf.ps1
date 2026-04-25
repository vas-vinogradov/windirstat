param(
    [string]$ExePath = (Join-Path $PSScriptRoot "..\build\WinDirStat_x64.exe"),
    [string]$RootPath = (Join-Path $PSScriptRoot "..\rust\scan_engine"),
    [int]$InboundQueueCapacity = 0,
    [int]$OutboundQueueCapacity = 0,
    [int]$WarmupRuns = 1,
    [int]$MeasuredRuns = 3,
    [int]$TimeoutSeconds = 600,
    [string]$OutputPath = "",
    [switch]$KeepLogs
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Intent: measure the real WinDirStat app using its quiet CSV command path and
# the RPC scan engine. This exercises NamedPipeRpcClient, unlike the direct
# PowerShell RPC tools.
# Contract: this script changes only process environment/configuration. It does
# not alter scan ownership, RPC protocol, or observer/model semantics.

function Resolve-ExistingPath {
    param([string]$Path)
    return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).ProviderPath
}

function Quote-ProcessArgument {
    param([string]$Value)
    return '"' + ($Value -replace '"', '\"') + '"'
}

function New-Workspace {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("windirstat-app-rpc-scan-perf-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N")))
    New-Item -ItemType Directory -Path $path | Out-Null
    return $path
}

function Get-FileHashText {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return ""
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
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

function Read-InboundQueueSummary {
    param(
        [string]$StdOut,
        [string]$StdErr,
        [string]$ClientLogFile
    )

    $clientLog = if (-not [string]::IsNullOrWhiteSpace($ClientLogFile) -and (Test-Path -LiteralPath $ClientLogFile)) {
        Get-Content -LiteralPath $ClientLogFile -Raw
    }
    else {
        ""
    }

    $text = @($StdOut, $StdErr, $clientLog) -join "`n"
    $match = [regex]::Match(
        $text,
        'InboundQueueSummary capacity=(\d+) enqueued=(\d+) processed=(\d+) maxDepth=(\d+) enqueueWaitMs=([0-9.]+) processingMs=([0-9.]+)')
    if (-not $match.Success) {
        return $null
    }

    return [pscustomobject][ordered]@{
        capacity = [int]$match.Groups[1].Value
        messagesEnqueued = [int]$match.Groups[2].Value
        messagesProcessed = [int]$match.Groups[3].Value
        maxDepth = [int]$match.Groups[4].Value
        enqueueWaitMs = [double]$match.Groups[5].Value
        processingMs = [double]$match.Groups[6].Value
    }
}

function Find-HostLogForAppProcess {
    param(
        [string]$Workspace,
        [int]$AppProcessId,
        [datetime]$StartedAt
    )

    $temp = [System.IO.Path]::GetTempPath()
    $pattern = "windirstat-scan-host-$AppProcessId-*.log"
    $candidates = @(Get-ChildItem -LiteralPath $temp -Filter $pattern -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTimeUtc -ge $StartedAt.ToUniversalTime().AddSeconds(-5) } |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($candidates.Count -eq 0) {
        return ""
    }

    $copyPath = Join-Path $Workspace ("host-{0}.log" -f $AppProcessId)
    Copy-Item -LiteralPath $candidates[0].FullName -Destination $copyPath -Force
    return $copyPath
}

function Get-WinDirStatProcesses {
    @(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -like "WinDirStat*" -or $_.ProcessName -like "windirstat-scan-host*" } |
        ForEach-Object {
            [pscustomobject]@{
                processName = $_.ProcessName
                id = $_.Id
                path = $_.Path
            }
        })
}

function Invoke-AppScanRun {
    param(
        [string]$ExePath,
        [string]$RootPath,
        [string]$Workspace,
        [int]$RunIndex,
        [bool]$Warmup,
        [int]$InboundQueueCapacity,
        [int]$OutboundQueueCapacity,
        [int]$TimeoutSeconds
    )

    $csvPath = Join-Path $Workspace ("scan-{0}.csv" -f $RunIndex)
    $stdoutPath = Join-Path $Workspace ("stdout-{0}.txt" -f $RunIndex)
    $stderrPath = Join-Path $Workspace ("stderr-{0}.txt" -f $RunIndex)
    $clientLogPath = Join-Path $Workspace ("client-{0}.log" -f $RunIndex)

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $ExePath
    $psi.Arguments = "{0} /savetocsv {1} --scan-engine rpc" -f `
        (Quote-ProcessArgument $RootPath),
        (Quote-ProcessArgument $csvPath)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables["WINDIRSTAT_SCAN_ENGINE"] = "rpc"
    $psi.EnvironmentVariables["WINDIRSTAT_SCAN_TIMING"] = "1"
    $psi.EnvironmentVariables["WINDIRSTAT_RUST_SCAN_THREADS"] = "8"
    $psi.EnvironmentVariables["WINDIRSTAT_DISABLE_ELEVATION"] = "1"
    $psi.EnvironmentVariables["WINDIRSTAT_RPC_CLIENT_LOG_FILE"] = $clientLogPath

    if ($InboundQueueCapacity -gt 0) {
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY"] = [string]$InboundQueueCapacity
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY")
    }

    if ($OutboundQueueCapacity -gt 0) {
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY"] = [string]$OutboundQueueCapacity
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY")
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $startedAt = Get-Date
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $stdout = ""
    $stderr = ""
    try {
        if (-not $process.Start()) {
            throw "Failed to start '$ExePath'."
        }

        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            $process.Kill()
            $process.WaitForExit()
        }

        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        Set-Content -LiteralPath $stdoutPath -Value $stdout -Encoding UTF8
        Set-Content -LiteralPath $stderrPath -Value $stderr -Encoding UTF8
    }
    finally {
        $timer.Stop()
    }

    $hostLog = Find-HostLogForAppProcess -Workspace $Workspace -AppProcessId $process.Id -StartedAt $startedAt
    $csvExists = Test-Path -LiteralPath $csvPath
    $csvItem = if ($csvExists) { Get-Item -LiteralPath $csvPath } else { $null }
    $lineCount = if ($csvExists) { @(Get-Content -LiteralPath $csvPath -ReadCount 1000).Count } else { 0 }
    $lingering = @(Get-WinDirStatProcesses | Where-Object { $_.id -ne $PID })
    $elevatedRelaunch = @($lingering | Where-Object { $_.processName -like "WinDirStat*" }).Count -gt 0

    $result = [pscustomobject][ordered]@{
        runIndex = $RunIndex
        warmup = $Warmup
        inboundQueueCapacity = $InboundQueueCapacity
        outboundQueueCapacity = $OutboundQueueCapacity
        timedOut = $timedOut
        exitCode = if ($timedOut) { $null } else { $process.ExitCode }
        succeeded = (-not $timedOut -and $process.ExitCode -eq 0 -and $csvExists)
        elapsedMs = [Math]::Round($timer.Elapsed.TotalMilliseconds, 2)
        csvPath = $csvPath
        csvExists = $csvExists
        csvBytes = if ($csvItem) { $csvItem.Length } else { 0 }
        csvSha256 = Get-FileHashText -Path $csvPath
        csvLineCount = $lineCount
        hostLog = $hostLog
        hostTiming = Read-HostTimingSummary -LogFile $hostLog
        clientLog = $clientLogPath
        inboundQueueSummary = Read-InboundQueueSummary -StdOut $stdout -StdErr $stderr -ClientLogFile $clientLogPath
        stdoutPath = $stdoutPath
        stderrPath = $stderrPath
        lingeringProcesses = $lingering
        anomaly = if ($elevatedRelaunch) { "possible-elevated-relaunch" } elseif ($lingering.Count -gt 0) { "lingering-processes" } else { "" }
    }

    $process.Dispose()
    return $result
}

function Get-Average {
    param([double[]]$Values)
    if ($Values.Count -eq 0) {
        return 0
    }
    $sum = 0.0
    foreach ($value in $Values) {
        $sum += $value
    }
    return [Math]::Round($sum / $Values.Count, 2)
}

$resolvedExe = Resolve-ExistingPath -Path $ExePath
$resolvedRoot = Resolve-ExistingPath -Path $RootPath
$workspace = New-Workspace
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("windirstat-app-rpc-scan-perf-{0}-{1}.json" -f $PID, ([Guid]::NewGuid().ToString("N")))
}

$runs = New-Object System.Collections.Generic.List[object]
$total = $WarmupRuns + $MeasuredRuns
for ($index = 0; $index -lt $total; $index++) {
    $isWarmup = $index -lt $WarmupRuns
    Write-Host ("[{0}/{1}] app rpc scan phase={2} inbound={3} outbound={4}" -f `
        ($index + 1), $total, ($(if ($isWarmup) { "warmup" } else { "measure" })), $InboundQueueCapacity, $OutboundQueueCapacity)
    $runs.Add((Invoke-AppScanRun `
        -ExePath $resolvedExe `
        -RootPath $resolvedRoot `
        -Workspace $workspace `
        -RunIndex ($index + 1) `
        -Warmup $isWarmup `
        -InboundQueueCapacity $InboundQueueCapacity `
        -OutboundQueueCapacity $OutboundQueueCapacity `
        -TimeoutSeconds $TimeoutSeconds))
}

$runArray = @($runs.ToArray())
$measured = @($runArray | Where-Object { -not $_.warmup })
$baselineHash = if ($measured.Count -gt 0) { [string]$measured[0].csvSha256 } else { "" }
foreach ($run in $measured) {
    if ([string]$run.csvSha256 -ne $baselineHash) {
        $run.anomaly = (($run.anomaly, "csv-hash-mismatch") | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join ";"
    }
}

$report = [pscustomobject][ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    exePath = $resolvedExe
    rootPath = $resolvedRoot
    inboundQueueCapacity = $InboundQueueCapacity
    outboundQueueCapacity = $OutboundQueueCapacity
    warmupRuns = $WarmupRuns
    measuredRuns = $MeasuredRuns
    timeoutSeconds = $TimeoutSeconds
    workspace = $workspace
    averageElapsedMs = Get-Average -Values ([double[]]@($measured | ForEach-Object { [double]$_.elapsedMs }))
    allSucceeded = @($runArray | Where-Object { -not $_.succeeded }).Count -eq 0
    baselineCsvSha256 = $baselineHash
    runs = $runArray
}

$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $OutputPath -Encoding UTF8

Write-Host ""
Write-Host "WinDirStat app RPC scan perf"
Write-Host ("  root: {0}" -f $resolvedRoot)
Write-Host ("  inbound={0} outbound={1} avgElapsedMs={2}" -f $InboundQueueCapacity, $OutboundQueueCapacity, $report.averageElapsedMs)
Write-Host ("  output: {0}" -f $OutputPath)
foreach ($run in $runArray) {
    Write-Host ("  run={0} warmup={1} ok={2} elapsedMs={3} csvBytes={4} anomaly={5}" -f `
        $run.runIndex, $run.warmup, $run.succeeded, $run.elapsedMs, $run.csvBytes, $run.anomaly)
}

if (-not $KeepLogs) {
    Remove-Item -LiteralPath $workspace -Recurse -Force
}
else {
    Write-Host ("  workspace: {0}" -f $workspace)
}

if (-not $report.allSucceeded) {
    exit 1
}
