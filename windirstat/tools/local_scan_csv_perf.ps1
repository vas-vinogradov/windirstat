param(
    [string]$AppPath = (Join-Path $PSScriptRoot "..\build\WinDirStat_x64.exe"),
    [string]$RootPath = (Join-Path $PSScriptRoot "..\rust\scan_engine"),
    [string]$OutputPath = "",
    [int]$TimeoutSeconds = 600,
    [uint64]$ProcessorAffinityMask = 0,
    [switch]$KeepCsv
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Intent: measure the original/local WinDirStat scanner through its quiet CSV
# command-line path. This is deliberately separate from RPC-host measurements
# because the original scanner lives in the UI process and uses CItem queues.

function Resolve-ExistingPath {
    param([string]$Path)

    return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).ProviderPath
}

function Quote-ProcessArgument {
    param([string]$Value)

    return '"' + ($Value -replace '"', '\"') + '"'
}

$resolvedAppPath = Resolve-ExistingPath -Path $AppPath
$resolvedRootPath = Resolve-ExistingPath -Path $RootPath
$workspace = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("windirstat-local-scan-csv-perf-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N")))
New-Item -ItemType Directory -Path $workspace | Out-Null

$csvPath = Join-Path $workspace "scan.csv"
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("windirstat-local-scan-csv-perf-{0}-{1}.json" -f $PID, ([Guid]::NewGuid().ToString("N")))
}

$signature = Get-AuthenticodeSignature -LiteralPath $resolvedAppPath -ErrorAction SilentlyContinue
$signatureStatus = if ($null -ne $signature) { [string]$signature.Status } else { "Unknown" }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $resolvedAppPath
$psi.Arguments = "{0} /savetocsv {1}" -f (Quote-ProcessArgument $resolvedRootPath), (Quote-ProcessArgument $csvPath)
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$process = New-Object System.Diagnostics.Process
$process.StartInfo = $psi
$timer = [System.Diagnostics.Stopwatch]::StartNew()
$timedOut = $false
try {
    if (-not $process.Start()) {
        throw "Failed to start '$resolvedAppPath'."
    }

    if ($ProcessorAffinityMask -ne 0) {
        $process.ProcessorAffinity = [IntPtr]([int64]$ProcessorAffinityMask)
    }

    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $timedOut = $true
        $process.Kill()
        $process.WaitForExit()
    }
}
finally {
    $timer.Stop()
}

$csvExists = Test-Path -LiteralPath $csvPath
$csvLength = if ($csvExists) { (Get-Item -LiteralPath $csvPath).Length } else { 0 }
$report = [pscustomobject][ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    appPath = $resolvedAppPath
    rootPath = $resolvedRootPath
    signature = $signatureStatus
    processorAffinityMask = if ($ProcessorAffinityMask -eq 0) { "default" } else { [string]$ProcessorAffinityMask }
    timeoutSeconds = $TimeoutSeconds
    timedOut = $timedOut
    exitCode = if ($timedOut) { $null } else { $process.ExitCode }
    succeeded = (-not $timedOut -and $process.ExitCode -eq 0)
    elapsedMs = [Math]::Round($timer.Elapsed.TotalMilliseconds, 2)
    csvPath = $csvPath
    csvBytes = $csvLength
}

$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host ("WinDirStat local CSV scan app='{0}' root='{1}' signature={2}" -f $resolvedAppPath, $resolvedRootPath, $signatureStatus)
Write-Host ("  exitCode={0} timedOut={1} elapsedMs={2} csvBytes={3}" -f $report.exitCode, $report.timedOut, $report.elapsedMs, $report.csvBytes)
Write-Host ("  output: {0}" -f $OutputPath)

$process.Dispose()
if (-not $KeepCsv -and (Test-Path -LiteralPath $workspace)) {
    Remove-Item -LiteralPath $workspace -Recurse -Force
}

if (-not $report.succeeded) {
    exit 1
}
