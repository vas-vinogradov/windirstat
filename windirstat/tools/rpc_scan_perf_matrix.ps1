param(
    [string]$RootPath = (Join-Path $PSScriptRoot "..\rust\scan_engine"),
    [string]$DebugHostPath = (Join-Path $PSScriptRoot "..\build\windirstat-scan-host_x64.exe"),
    [string]$ReleaseHostPath = "",
    [string[]]$Modes = @("rust", "legacy"),
    [int]$RepeatCount = 3,
    [int]$WarmupCount = 1,
    [int]$TimeoutSeconds = 180,
    [int]$MaxEvents = 50000,
    [string[]]$RustScanThreadCounts = @(),
    [string]$OutputPath = "",
    [switch]$IncludeVerboseHostLogs,
    [switch]$IncludeSingleThread,
    [switch]$EnableTiming,
    [switch]$IncludeBatchProgress,
    [int[]]$BatchProgressSizes = @(64),
    [int]$BatchProgressMaxDelayMs = 10,
    [int[]]$OutboundQueueCapacities = @(0),
    [int]$InboundQueueCapacity = 0,
    [switch]$KeepLogs
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Intent: run a controlled scan-host performance matrix without changing scan
# architecture. This keeps measurements grouped by host binary, engine mode,
# logging level, and process affinity so we can find where time was introduced
# before doing any Rust optimization work.

function Resolve-OptionalPath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    if (-not (Test-Path -LiteralPath $Path)) {
        return ""
    }

    return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).ProviderPath
}

function Invoke-CompareRun {
    param(
        [string]$CompareScript,
        [string]$HostPath,
        [string]$RootPath,
        [string]$Mode,
        [bool]$VerboseHostLogs,
        [uint64]$ProcessorAffinityMask,
        [int]$RustScanThreads,
        [int]$TimeoutSeconds,
        [int]$MaxEvents,
        [string]$OutputPath,
        [bool]$EnableTiming,
        [bool]$BatchProgress,
        [int]$BatchProgressSize,
        [int]$BatchProgressMaxDelayMs,
        [int]$OutboundQueueCapacity,
        [int]$InboundQueueCapacity,
        [bool]$KeepLogs
    )

    $arguments = @(
        "-NoLogo",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $CompareScript,
        "-HostPath",
        $HostPath,
        "-RootPath",
        $RootPath,
        "-Modes",
        $Mode,
        "-TimeoutSeconds",
        ([string]$TimeoutSeconds),
        "-MaxEvents",
        ([string]$MaxEvents),
        "-ProcessorAffinityMask",
        ([string]$ProcessorAffinityMask),
        "-RustScanThreads",
        ([string]$RustScanThreads),
        "-OutputPath",
        $OutputPath
    )

    if ($VerboseHostLogs) {
        $arguments += "-VerboseHostLogs"
    }

    if ($EnableTiming) {
        $arguments += "-EnableTiming"
    }

    if ($BatchProgress) {
        $arguments += @(
            "-BatchProgress",
            "-BatchProgressSize",
            ([string]$BatchProgressSize),
            "-BatchProgressMaxDelayMs",
            ([string]$BatchProgressMaxDelayMs)
        )
    }

    if ($OutboundQueueCapacity -gt 0) {
        $arguments += @("-OutboundQueueCapacity", ([string]$OutboundQueueCapacity))
    }

    if ($InboundQueueCapacity -gt 0) {
        $arguments += @("-InboundQueueCapacity", ([string]$InboundQueueCapacity))
    }

    if ($KeepLogs) {
        $arguments += "-KeepLogs"
    }

    $output = & powershell @arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "rpc_scan_compare.ps1 failed with exit code $exitCode. Output: $($output -join "`n")"
    }

    $report = Get-Content -LiteralPath $OutputPath -Raw | ConvertFrom-Json
    return $report.results[0]
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

function Get-Minimum {
    param([double[]]$Values)

    if ($Values.Count -eq 0) {
        return 0
    }

    $minimum = $Values[0]
    foreach ($value in $Values) {
        if ($value -lt $minimum) {
            $minimum = $value
        }
    }

    return [Math]::Round($minimum, 2)
}

function Get-Maximum {
    param([double[]]$Values)

    if ($Values.Count -eq 0) {
        return 0
    }

    $maximum = $Values[0]
    foreach ($value in $Values) {
        if ($value -gt $maximum) {
            $maximum = $value
        }
    }

    return [Math]::Round($maximum, 2)
}

function New-SummaryRows {
    param([object[]]$Runs)

    $groups = $Runs | Group-Object hostLabel, mode, logging, affinity, rustScanThreads, batchProgress
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($group in $groups) {
        $items = @($group.Group)
        $first = $items[0]
        $elapsed = [double[]]@($items | ForEach-Object { [double]$_.elapsedMs })
        $dirsPerSecond = [double[]]@($items | ForEach-Object { [double]$_.directoriesPerSecond })
        $entriesPerSecond = [double[]]@($items | ForEach-Object { [double]$_.entriesPerSecond })

        $rows.Add([pscustomobject][ordered]@{
            hostLabel = $first.hostLabel
            mode = $first.mode
            logging = $first.logging
            affinity = $first.affinity
            rustScanThreads = $first.rustScanThreads
            batchProgress = $first.batchProgress
            repeats = $items.Count
            terminalKind = $first.terminalKind
            failures = @($items | Where-Object { $_.failureCount -gt 0 }).Count
            progressEventCount = $first.progressEventCount
            physicalRpcMessageCount = $first.physicalRpcMessageCount
            batchMessageCount = $first.batchMessageCount
            averageBatchSize = $first.averageBatchSize
            maxBatchSize = $first.maxBatchSize
            directoriesVisited = $first.directoriesVisited
            childEntriesObserved = $first.childEntriesObserved
            totalLogicalSize = $first.totalLogicalSize
            totalPhysicalSize = $first.totalPhysicalSize
            elapsedMsAvg = Get-Average -Values $elapsed
            elapsedMsMin = Get-Minimum -Values $elapsed
            elapsedMsMax = Get-Maximum -Values $elapsed
            directoriesPerSecondAvg = Get-Average -Values $dirsPerSecond
            entriesPerSecondAvg = Get-Average -Values $entriesPerSecond
        })
    }

    return @($rows.ToArray())
}

$compareScript = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "rpc_scan_compare.ps1") -ErrorAction Stop
$resolvedRoot = (Resolve-Path -LiteralPath $RootPath -ErrorAction Stop).ProviderPath
$resolvedDebugHost = Resolve-OptionalPath -Path $DebugHostPath
$resolvedReleaseHost = Resolve-OptionalPath -Path $ReleaseHostPath

$hostVariants = New-Object System.Collections.Generic.List[object]
if (-not [string]::IsNullOrWhiteSpace($resolvedDebugHost)) {
    $hostVariants.Add([pscustomobject]@{ label = "debug"; path = $resolvedDebugHost })
}

if (-not [string]::IsNullOrWhiteSpace($resolvedReleaseHost)) {
    $hostVariants.Add([pscustomobject]@{ label = "release"; path = $resolvedReleaseHost })
}

if ($hostVariants.Count -eq 0) {
    throw "No host binaries were found. Provide -DebugHostPath and/or -ReleaseHostPath."
}

$loggingVariants = @("milestone")
if ($IncludeVerboseHostLogs) {
    $loggingVariants += "verbose"
}

$normalizedModes = @(
    $Modes | ForEach-Object {
        ([string]$_).Split(",", [System.StringSplitOptions]::RemoveEmptyEntries) |
            ForEach-Object { $_.Trim().ToLowerInvariant() }
    }
)

$affinityVariants = @(
    [pscustomobject]@{ label = "default"; mask = [uint64]0 }
)

$rustThreadVariants = @([pscustomobject]@{ label = "default"; count = 0 })
foreach ($countValue in $RustScanThreadCounts) {
    $counts = ([string]$countValue).Split(",", [System.StringSplitOptions]::RemoveEmptyEntries)
    foreach ($countText in $counts) {
        $count = [int]$countText.Trim()
        if ($count -gt 0) {
            $rustThreadVariants += [pscustomobject]@{ label = [string]$count; count = [int]$count }
        }
    }
}
if ($IncludeSingleThread) {
    $affinityVariants += [pscustomobject]@{ label = "single-thread-affinity"; mask = [uint64]1 }
}

$batchVariants = @([pscustomobject]@{ label = "off"; enabled = $false; size = 0 })
if ($IncludeBatchProgress) {
    foreach ($batchSize in $BatchProgressSizes) {
        if ($batchSize -gt 0) {
            $batchVariants += [pscustomobject]@{ label = "on-$batchSize"; enabled = $true; size = [int]$batchSize }
        }
    }
}

$outboundQueueVariants = @()
foreach ($capacity in $OutboundQueueCapacities) {
    if ($capacity -ge 0) {
        $label = if ($capacity -eq 0) { "off" } else { [string]$capacity }
        $outboundQueueVariants += [pscustomobject]@{ label = $label; capacity = [int]$capacity }
    }
}
if ($outboundQueueVariants.Count -eq 0) {
    $outboundQueueVariants = @([pscustomobject]@{ label = "off"; capacity = 0 })
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("windirstat-rpc-scan-perf-matrix-{0}-{1}.json" -f $PID, ([Guid]::NewGuid().ToString("N")))
}
else {
    $parent = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent | Out-Null
    }
}

$workspace = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("windirstat-rpc-scan-perf-matrix-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N")))
New-Item -ItemType Directory -Path $workspace | Out-Null

$runs = New-Object System.Collections.Generic.List[object]
$totalIterations = $hostVariants.Count * $normalizedModes.Count * $loggingVariants.Count * $affinityVariants.Count * $rustThreadVariants.Count * $batchVariants.Count * $outboundQueueVariants.Count * ($WarmupCount + $RepeatCount)
$iteration = 0

foreach ($hostVariant in $hostVariants) {
    foreach ($mode in $normalizedModes) {
        foreach ($logging in $loggingVariants) {
            foreach ($affinity in $affinityVariants) {
                foreach ($rustThreads in $rustThreadVariants) {
                    foreach ($batch in $batchVariants) {
                    foreach ($outboundQueue in $outboundQueueVariants) {
                        for ($index = 0; $index -lt ($WarmupCount + $RepeatCount); $index++) {
                            $iteration++
                            $isWarmup = $index -lt $WarmupCount
                            $phase = if ($isWarmup) { "warmup" } else { "measure" }
                            Write-Host ("[{0}/{1}] {2} host={3} mode={4} logging={5} affinity={6} rustThreads={7} batch={8} outboundQueue={9}" -f `
                                $iteration,
                                $totalIterations,
                                $phase,
                                $hostVariant.label,
                                $mode,
                                $logging,
                                $affinity.label,
                                $rustThreads.label,
                                $batch.label,
                                $outboundQueue.label)

                            $runOutputPath = Join-Path $workspace ("compare-{0}.json" -f $iteration)
                            $result = Invoke-CompareRun `
                                -CompareScript $compareScript.ProviderPath `
                                -HostPath $hostVariant.path `
                                -RootPath $resolvedRoot `
                                -Mode $mode `
                                -VerboseHostLogs ($logging -eq "verbose") `
                                -ProcessorAffinityMask ([uint64]$affinity.mask) `
                                -RustScanThreads ([int]$rustThreads.count) `
                                -TimeoutSeconds $TimeoutSeconds `
                                -MaxEvents $MaxEvents `
                                -OutputPath $runOutputPath `
                                -EnableTiming ([bool]$EnableTiming) `
                                -BatchProgress ([bool]$batch.enabled) `
                                -BatchProgressSize ([int]$batch.size) `
                                -BatchProgressMaxDelayMs $BatchProgressMaxDelayMs `
                                -OutboundQueueCapacity ([int]$outboundQueue.capacity) `
                                -InboundQueueCapacity $InboundQueueCapacity `
                                -KeepLogs ([bool]$KeepLogs)

                            if (-not $isWarmup) {
                                $runs.Add([pscustomobject][ordered]@{
                                    hostLabel = $hostVariant.label
                                    hostPath = $hostVariant.path
                                    mode = [string]$result.mode
                                    logging = $logging
                                    affinity = $affinity.label
                                    affinityMask = [uint64]$affinity.mask
                                    rustScanThreads = $rustThreads.label
                                    batchProgress = $batch.label
                                    outboundQueue = $outboundQueue.label
                                    terminalKind = [string]$result.terminalKind
                                    failureCount = [int]$result.failureCount
                                    progressEventCount = [int]$result.progressEventCount
                                    physicalRpcMessageCount = [int]$result.physicalRpcMessageCount
                                    batchMessageCount = [int]$result.batchMessageCount
                                    averageBatchSize = [double]$result.averageBatchSize
                                    maxBatchSize = [int]$result.maxBatchSize
                                    directoriesVisited = [int]$result.directoriesVisited
                                    childEntriesObserved = [int]$result.childEntriesObserved
                                    totalLogicalSize = [uint64]$result.totalLogicalSize
                                    totalPhysicalSize = [uint64]$result.totalPhysicalSize
                                    elapsedMs = [double]$result.elapsedMs
                                    hostTiming = $result.hostTiming
                                    timingBreakdown = $result.timingBreakdown
                                    directoriesPerSecond = [double]$result.directoriesPerSecond
                                    entriesPerSecond = [double]$result.entriesPerSecond
                                    logicalBytesPerSecond = [double]$result.logicalBytesPerSecond
                                    sourceReport = $runOutputPath
                                })
                            }
                        }
                    }
                    }
                }
            }
        }
    }
}

$runArray = @($runs.ToArray())
$summary = New-SummaryRows -Runs $runArray
$report = [pscustomobject][ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    rootPath = $resolvedRoot
    repeatCount = $RepeatCount
    warmupCount = $WarmupCount
    timeoutSeconds = $TimeoutSeconds
    maxEvents = $MaxEvents
    timingEnabled = [bool]$EnableTiming
    hostVariants = @($hostVariants.ToArray())
    modes = @($normalizedModes)
    loggingVariants = @($loggingVariants)
    affinityVariants = @($affinityVariants)
    rustThreadVariants = @($rustThreadVariants)
    batchVariants = @($batchVariants)
    batchProgressMaxDelayMs = $BatchProgressMaxDelayMs
    runs = $runArray
    summary = $summary
}

$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $OutputPath -Encoding UTF8

Write-Host ""
Write-Host "WinDirStat RPC scan performance matrix"
Write-Host ("  root: {0}" -f $resolvedRoot)
Write-Host ("  output: {0}" -f $OutputPath)
Write-Host ""
foreach ($row in $summary) {
    Write-Host ("[{0}/{1}/{2}/{3}/threads={4}/batch={5}] repeats={6} terminal={7} failures={8} elapsedAvgMs={9} elapsedMinMs={10} dirs/s={11} entries/s={12}" -f `
        $row.hostLabel,
        $row.mode,
        $row.logging,
        $row.affinity,
        $row.rustScanThreads,
        $row.batchProgress,
        $row.repeats,
        $row.terminalKind,
        $row.failures,
        $row.elapsedMsAvg,
        $row.elapsedMsMin,
        $row.directoriesPerSecondAvg,
        $row.entriesPerSecondAvg)
}

if (-not $KeepLogs) {
    Remove-Item -LiteralPath $workspace -Recurse -Force
}
else {
    Write-Host ("Preserved workspace: {0}" -f $workspace)
}
