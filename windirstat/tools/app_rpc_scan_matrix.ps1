param(
    [string]$ExePath = (Join-Path $PSScriptRoot "..\build\WinDirStat_x64.exe"),
    [string]$RootPath = "C:\Users\vasil\OneDrive\Documents\Projects",
    [int[]]$InboundQueueCapacities = @(0, 64, 256),
    [int[]]$OutboundQueueCapacities = @(0, 64),
    [int]$WarmupRuns = 1,
    [int]$MeasuredRuns = 3,
    [int]$TimeoutSeconds = 600,
    [string]$OutputPath = "",
    [switch]$KeepLogs
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Intent: run a reproducible matrix against the real WinDirStat app so
# NamedPipeRpcClient inbound queue behavior is measured through the production
# RPC scan path.

function Resolve-ExistingPath {
    param([string]$Path)
    return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).ProviderPath
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

function Get-JsonProperty {
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

$perfScript = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "app_rpc_scan_perf.ps1") -ErrorAction Stop
$resolvedExe = Resolve-ExistingPath -Path $ExePath
$resolvedRoot = Resolve-ExistingPath -Path $RootPath

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("windirstat-app-rpc-scan-matrix-{0}-{1}.json" -f $PID, ([Guid]::NewGuid().ToString("N")))
}

$workspace = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("windirstat-app-rpc-scan-matrix-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N")))
New-Item -ItemType Directory -Path $workspace | Out-Null

$runs = New-Object System.Collections.Generic.List[object]
$summary = New-Object System.Collections.Generic.List[object]
$total = $InboundQueueCapacities.Count * $OutboundQueueCapacities.Count
$iteration = 0

foreach ($inbound in $InboundQueueCapacities) {
    foreach ($outbound in $OutboundQueueCapacities) {
        $iteration++
        Write-Host ("[{0}/{1}] inbound={2} outbound={3}" -f $iteration, $total, $inbound, $outbound)
        $comboOutput = Join-Path $workspace ("combo-in{0}-out{1}.json" -f $inbound, $outbound)
        $arguments = @(
            "-NoLogo",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            $perfScript.ProviderPath,
            "-ExePath",
            $resolvedExe,
            "-RootPath",
            $resolvedRoot,
            "-InboundQueueCapacity",
            ([string]$inbound),
            "-OutboundQueueCapacity",
            ([string]$outbound),
            "-WarmupRuns",
            ([string]$WarmupRuns),
            "-MeasuredRuns",
            ([string]$MeasuredRuns),
            "-TimeoutSeconds",
            ([string]$TimeoutSeconds),
            "-OutputPath",
            $comboOutput
        )
        if ($KeepLogs) {
            $arguments += "-KeepLogs"
        }

        $output = & powershell @arguments
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            throw "app_rpc_scan_perf.ps1 failed for inbound=$inbound outbound=$outbound with exit code $exitCode. Output: $($output -join "`n")"
        }

        $report = Get-Content -LiteralPath $comboOutput -Raw | ConvertFrom-Json
        $measured = @($report.runs | Where-Object { -not $_.warmup })
        $elapsed = [double[]]@($measured | ForEach-Object { [double]$_.elapsedMs })
        $hostWrite = [double[]]@($measured | ForEach-Object {
            $timing = Get-JsonProperty -Object $_ -Name "hostTiming"
            [double](Get-JsonProperty -Object $timing -Name "rpcWriteMs")
        })
        $hostEnqueue = [double[]]@($measured | ForEach-Object {
            $timing = Get-JsonProperty -Object $_ -Name "hostTiming"
            [double](Get-JsonProperty -Object $timing -Name "rpcEnqueueWaitMs")
        })
        $maxInboundDepth = 0
        foreach ($run in $measured) {
            $queue = Get-JsonProperty -Object $run -Name "inboundQueueSummary"
            $depth = [int](Get-JsonProperty -Object $queue -Name "maxDepth")
            if ($depth -gt $maxInboundDepth) {
                $maxInboundDepth = $depth
            }
        }

        $row = [pscustomobject][ordered]@{
            inboundQueueCapacity = $inbound
            outboundQueueCapacity = $outbound
            measuredRuns = $measured.Count
            allSucceeded = [bool]$report.allSucceeded
            averageElapsedMs = Get-Average -Values $elapsed
            averageHostPipeWriteMs = Get-Average -Values $hostWrite
            averageHostEnqueueWaitMs = Get-Average -Values $hostEnqueue
            maxInboundQueueDepth = $maxInboundDepth
            reportPath = $comboOutput
            notes = if (-not [bool]$report.allSucceeded) { "failure" } elseif ($maxInboundDepth -eq 0 -and $inbound -gt 0) { "no inbound trace in release build" } else { "" }
        }
        $summary.Add($row)
        $runs.Add($report)
    }
}

$summaryArray = @($summary.ToArray())
$reportOut = [pscustomobject][ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    exePath = $resolvedExe
    rootPath = $resolvedRoot
    warmupRuns = $WarmupRuns
    measuredRuns = $MeasuredRuns
    timeoutSeconds = $TimeoutSeconds
    workspace = $workspace
    summary = $summaryArray
    reports = @($runs.ToArray())
}

$reportOut | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $OutputPath -Encoding UTF8

Write-Host ""
Write-Host "WinDirStat app RPC scan matrix"
Write-Host ("  root: {0}" -f $resolvedRoot)
Write-Host ("  output: {0}" -f $OutputPath)
Write-Host ""
$summaryArray |
    Sort-Object inboundQueueCapacity, outboundQueueCapacity |
    Format-Table inboundQueueCapacity, outboundQueueCapacity, averageElapsedMs, averageHostPipeWriteMs, averageHostEnqueueWaitMs, maxInboundQueueDepth, notes -AutoSize

if (-not $KeepLogs) {
    Remove-Item -LiteralPath $workspace -Recurse -Force
}
else {
    Write-Host ("Preserved workspace: {0}" -f $workspace)
}
