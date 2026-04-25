param(
    [string]$HostPath = (Join-Path $PSScriptRoot "..\build\windirstat-scan-host_x64.exe"),
    [switch]$BatchProgress,
    [int]$OutboundQueueCapacity = 0,
    [switch]$KeepFixtures
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Intent: minimal end-to-end RPC scan verification for the transitional host.
# Contract: Rust remains the authoritative scan engine; this script only speaks
# the existing named-pipe protocol and validates that the host forwards events.
# Temporary: keep this as a lightweight client-side smoke until the final UI
# test story is in place.

$script:RequestTimeoutMs = 15000

function Resolve-ExistingPath {
    param([string]$Path)

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return $resolved.ProviderPath
}

function Quote-ProcessArgument {
    param([string]$Value)

    return '"' + ($Value -replace '"', '\"') + '"'
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

function New-TempWorkspace {
    $name = "windirstat-rpc-scan-smoke-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N"))
    $path = Join-Path ([System.IO.Path]::GetTempPath()) $name
    New-Item -ItemType Directory -Path $path | Out-Null
    return $path
}

function Start-ScanHost {
    param(
        [string]$HostPath,
        [string]$PipeName,
        [string]$LogFile,
        [bool]$BatchProgress,
        [int]$OutboundQueueCapacity
    )

    $signature = Get-AuthenticodeSignature -LiteralPath $HostPath -ErrorAction SilentlyContinue
    $signatureStatus = if ($null -ne $signature) { [string]$signature.Status } else { "Unknown" }
    Write-Host ("Launching scan host path='{0}' signature={1}" -f $HostPath, $signatureStatus)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $HostPath
    $psi.Arguments = "--pipe {0} --log-file {1} --log-level verbose" -f `
        (Quote-ProcessArgument "\\.\pipe\$PipeName"), `
        (Quote-ProcessArgument $LogFile)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables.Remove("WINDIRSTAT_REMOTE_DISCOVERY_ENGINE")
    if ($BatchProgress) {
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_BATCH_PROGRESS"] = "1"
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_BATCH_SIZE"] = "64"
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_BATCH_PROGRESS")
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_BATCH_SIZE")
    }

    if ($OutboundQueueCapacity -gt 0) {
        $psi.EnvironmentVariables["WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY"] = [string]$OutboundQueueCapacity
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY")
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    try {
        if (-not $process.Start()) {
            throw "Failed to start scan host '$HostPath'."
        }
    }
    catch {
        throw ("Failed to start scan host '{0}'. Signature status={1}. On this machine, Smart App Control blocks the self-signed dev certificate, so use an unsigned debug build if needed." -f $HostPath, $signatureStatus)
    }

    return [pscustomobject]@{
        Process = $process
        SignatureStatus = $signatureStatus
    }
}

function Invoke-RpcScan {
    param(
        [System.IO.Stream]$Stream,
        [uint64]$RequestId,
        [string]$RootPath,
        [string]$ChildPath
    )

    Write-RpcJson -Stream $Stream -Message ([pscustomobject][ordered]@{
        kind = "StartScanRequest"
        requestId = $RequestId
        rootPath = $RootPath
        followMountPoints = $false
        followSymbolicLinks = $false
        followJunctions = $false
    })

    Write-RpcJson -Stream $Stream -Message ([pscustomobject][ordered]@{
        kind = "CloseRequestInput"
        requestId = $RequestId
    })

    $progressEvents = New-Object System.Collections.Generic.List[object]
    $physicalRpcMessages = 0
    $batchMessages = 0
    $terminal = $null
    while ($null -eq $terminal) {
        $message = Read-RpcJson -Stream $Stream
        if ($null -eq $message) {
            continue
        }

        if ([uint64]$message.requestId -ne $RequestId) {
            continue
        }

        $physicalRpcMessages++
        switch ([string]$message.kind) {
            "DirectoryProgressEvent" {
                $progressEvents.Add($message)
                continue
            }
            "DirectoryProgressBatchEvent" {
                $batchMessages++
                foreach ($item in @($message.items)) {
                    if ([uint64]$item.requestId -eq $RequestId) {
                        $progressEvents.Add($item)
                    }
                }
                continue
            }
            "ScanCompletedEvent" {
                $terminal = $message
                continue
            }
            "ScanCanceledEvent" {
                throw "Scan was canceled unexpectedly for request $RequestId."
            }
            "ScanFailedEvent" {
                throw "Scan failed for request $RequestId at '$($message.path)': $($message.message)"
            }
            default {
                throw "Unexpected RPC event kind '$($message.kind)'."
            }
        }
    }

    $visitedPaths = @($progressEvents | ForEach-Object { [string]$_.directoryPath })
    if (-not ($visitedPaths -contains $RootPath)) {
        throw "Expected a DirectoryProgressEvent for root '$RootPath'."
    }

    if (-not ($visitedPaths -contains $ChildPath)) {
        throw "Expected a DirectoryProgressEvent for child '$ChildPath'."
    }

    $rootProgress = $progressEvents | Where-Object { [string]$_.directoryPath -eq $RootPath } | Select-Object -First 1
    if ($null -eq $rootProgress) {
        throw "Missing root progress payload for '$RootPath'."
    }

    $rootFileNames = @($rootProgress.files | ForEach-Object { [string]$_.name })
    $rootDirectoryNames = @($rootProgress.directories | ForEach-Object { [string]$_.name })
    if (-not ($rootFileNames -contains "alpha.txt")) {
        throw "Root progress payload did not include alpha.txt."
    }

    if (-not ($rootDirectoryNames -contains "child")) {
        throw "Root progress payload did not include child directory."
    }

    return [pscustomobject][ordered]@{
        requestId = $RequestId
        terminalKind = [string]$terminal.kind
        progressEventCount = $progressEvents.Count
        physicalRpcMessageCount = $physicalRpcMessages
        batchMessageCount = $batchMessages
        visitedDirectories = $visitedPaths
        rootFiles = $rootFileNames
        rootDirectories = $rootDirectoryNames
    }
}

$resolvedHostPath = Resolve-ExistingPath -Path $HostPath
$workspaceRoot = New-TempWorkspace
$fixtureRoot = Join-Path $workspaceRoot "fixture"
$childPath = Join-Path $fixtureRoot "child"
$hostLogPath = Join-Path $workspaceRoot "host.log"
$scanHost = $null
$stream = $null

try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    New-Item -ItemType Directory -Path $childPath | Out-Null
    Set-Content -LiteralPath (Join-Path $fixtureRoot "alpha.txt") -Value "alpha" -NoNewline
    Set-Content -LiteralPath (Join-Path $childPath "beta.txt") -Value "beta" -NoNewline

    $pipeName = "WinDirStat.RpcScanSmoke.{0}.{1}" -f $PID, ([Guid]::NewGuid().ToString("N"))
    $scanHost = Start-ScanHost -HostPath $resolvedHostPath -PipeName $pipeName -LogFile $hostLogPath -BatchProgress ([bool]$BatchProgress) -OutboundQueueCapacity $OutboundQueueCapacity
    $stream = New-Object System.IO.Pipes.NamedPipeClientStream(
        ".",
        $pipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    $stream.Connect($script:RequestTimeoutMs)
    if ($stream.CanTimeout) {
        $stream.ReadTimeout = $script:RequestTimeoutMs
        $stream.WriteTimeout = $script:RequestTimeoutMs
    }

    $result = Invoke-RpcScan -Stream $stream -RequestId ([uint64]1) -RootPath $fixtureRoot -ChildPath $childPath
    [pscustomobject][ordered]@{
        generatedAt = (Get-Date).ToUniversalTime().ToString("o")
        hostPath = $resolvedHostPath
        signature = [string]$scanHost.SignatureStatus
        fixtureRoot = $fixtureRoot
        logFile = $hostLogPath
        requestId = $result.requestId
        terminalKind = $result.terminalKind
        progressEventCount = $result.progressEventCount
        physicalRpcMessageCount = $result.physicalRpcMessageCount
        batchMessageCount = $result.batchMessageCount
        visitedDirectories = $result.visitedDirectories
        rootFiles = $result.rootFiles
        rootDirectories = $result.rootDirectories
    } | ConvertTo-Json -Depth 10
}
finally {
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

    if (-not $KeepFixtures -and (Test-Path -LiteralPath $workspaceRoot)) {
        Remove-Item -LiteralPath $workspaceRoot -Recurse -Force
    }
}
