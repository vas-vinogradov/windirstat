param(
    [string]$HostPath = (Join-Path $PSScriptRoot "..\Build\windirstat-scan-host_x64.exe"),
    [string]$OutputPath = "",
    [switch]$KeepFixtures
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Temporary migration scaffolding: this exercises the scan-host RPC adapter
# around both the Rust-owned engine path and the legacy fallback path without
# automating the UI. Keep it separate from the Rust discovery contract harness
# and remove it once legacy discovery is retired.

$script:RequestTimeoutMs = 15000

function Resolve-SmokeHostPath {
    param([string]$Path)

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return $resolved.ProviderPath
}

function Quote-ProcessArgument {
    param([string]$Value)

    return '"' + ($Value -replace '"', '\"') + '"'
}

function New-SmokeTempRoot {
    $name = "windirstat-host-discovery-smoke-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N"))
    $path = Join-Path ([System.IO.Path]::GetTempPath()) $name
    New-Item -ItemType Directory -Path $path | Out-Null
    return $path
}

function Convert-ToUInt64 {
    param($Value)

    if ($null -eq $Value) {
        return [uint64]0
    }

    return [uint64]::Parse(([string]$Value), [System.Globalization.CultureInfo]::InvariantCulture)
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

function Ensure-SparseFileNative {
    if ("WinDirStatSmokeSparseFileNative" -as [type]) {
        return
    }

    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class WinDirStatSmokeSparseFileNative
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(
        IntPtr hDevice,
        uint dwIoControlCode,
        IntPtr lpInBuffer,
        uint nInBufferSize,
        IntPtr lpOutBuffer,
        uint nOutBufferSize,
        out uint lpBytesReturned,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern uint GetCompressedFileSizeW(
        string lpFileName,
        out uint lpFileSizeHigh);
}
"@
}

function New-SparseFixtureFile {
    param(
        [string]$Path,
        [uint64]$LogicalSize
    )

    Ensure-SparseFileNative
    $fs = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::ReadWrite,
        ([System.IO.FileShare]([int][System.IO.FileShare]::ReadWrite -bor [int][System.IO.FileShare]::Delete)))
    try {
        $returned = [uint32]0
        $ok = [WinDirStatSmokeSparseFileNative]::DeviceIoControl(
            $fs.SafeFileHandle.DangerousGetHandle(),
            [uint32]0x000900C4,
            [IntPtr]::Zero,
            [uint32]0,
            [IntPtr]::Zero,
            [uint32]0,
            [ref]$returned,
            [IntPtr]::Zero)
        if (-not $ok) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "FSCTL_SET_SPARSE failed for '$Path' with Win32 error $errorCode."
        }

        $fs.SetLength([int64]$LogicalSize)
        $fs.Flush()
    }
    finally {
        $fs.Dispose()
    }
}

function New-DiscoverySmokeFixtures {
    param([string]$Root)

    $fixtures = @()

    $basic = Join-Path $Root "basic"
    New-Item -ItemType Directory -Path $basic | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $basic "alpha.txt"), [byte[]](0x61, 0x6c, 0x70, 0x68, 0x61, 0x0a))
    New-Item -ItemType Directory -Path (Join-Path $basic "child") | Out-Null
    $fixtures += [pscustomobject][ordered]@{
        name = "basic"
        path = $basic
        description = "one regular file and one immediate child directory"
    }

    $hardlinks = Join-Path $Root "hardlinks"
    New-Item -ItemType Directory -Path $hardlinks | Out-Null
    $original = Join-Path $hardlinks "original.bin"
    $linked = Join-Path $hardlinks "linked.bin"
    [System.IO.File]::WriteAllBytes($original, [byte[]](0x68, 0x61, 0x72, 0x64, 0x6c, 0x69, 0x6e, 0x6b))
    New-Item -ItemType HardLink -Path $linked -Target $original | Out-Null
    $fixtures += [pscustomobject][ordered]@{
        name = "hardlinks"
        path = $hardlinks
        description = "two directory entries sharing one file identity"
    }

    $physical = Join-Path $Root "physical-size"
    New-Item -ItemType Directory -Path $physical | Out-Null
    New-SparseFixtureFile -Path (Join-Path $physical "sparse-1m.bin") -LogicalSize ([uint64](1024 * 1024))
    $fixtures += [pscustomobject][ordered]@{
        name = "physical-size"
        path = $physical
        description = "sparse file with logical size distinct from allocated size"
    }

    return $fixtures
}

function New-NormalizedEntry {
    param(
        [string]$Type,
        $Entry
    )

    if ($Type -eq "file") {
        return [pscustomobject][ordered]@{
            type = "file"
            name = [string]$Entry.name
            index = Convert-ToUInt64 $Entry.index
            sizeLogical = Convert-ToUInt64 $Entry.sizeLogical
            sizePhysical = Convert-ToUInt64 $Entry.sizePhysical
        }
    }

    return [pscustomobject][ordered]@{
        type = "directory"
        name = [string]$Entry.name
        index = Convert-ToUInt64 $Entry.index
        sizeLogical = $null
        sizePhysical = $null
    }
}

function Normalize-DiscoveryProgress {
    param($Progress)

    $entries = @()
    foreach ($directory in @($Progress.directories)) {
        $entries += New-NormalizedEntry -Type "directory" -Entry $directory
    }
    foreach ($file in @($Progress.files)) {
        $entries += New-NormalizedEntry -Type "file" -Entry $file
    }

    return @($entries | Sort-Object -Property type, name)
}

function Get-ExpectedPhysicalSize {
    param([string]$Path)

    Ensure-SparseFileNative
    $sizeHigh = [uint32]0
    $sizeLow = [WinDirStatSmokeSparseFileNative]::GetCompressedFileSizeW($Path, [ref]$sizeHigh)
    if ($sizeLow -eq [uint32]::MaxValue) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($errorCode -ne 0) {
            throw "GetCompressedFileSizeW failed for '$Path' with Win32 error $errorCode."
        }
    }

    return ([uint64]$sizeHigh * [uint64]0x100000000) + [uint64]$sizeLow
}

function Get-ExpectedDiscoveryEntries {
    param($Fixture)

    $entries = @()
    foreach ($directoryPath in [System.IO.Directory]::EnumerateDirectories([string]$Fixture.path)) {
        $entries += [pscustomobject][ordered]@{
            type = "directory"
            name = [System.IO.Path]::GetFileName($directoryPath)
            sizeLogical = $null
            sizePhysical = $null
            requiresFileIndex = $false
        }
    }

    foreach ($filePath in [System.IO.Directory]::EnumerateFiles([string]$Fixture.path)) {
        $fileInfo = Get-Item -LiteralPath $filePath
        $entries += [pscustomobject][ordered]@{
            type = "file"
            name = [System.IO.Path]::GetFileName($filePath)
            sizeLogical = [uint64]$fileInfo.Length
            sizePhysical = Get-ExpectedPhysicalSize -Path $filePath
            requiresFileIndex = $true
        }
    }

    return @($entries | Sort-Object -Property type, name)
}

function New-ComparisonFinding {
    param(
        [string]$Outcome,
        [string]$Entry,
        [string]$Field,
        $Expected,
        $Rust,
        $Legacy,
        [string]$Message
    )

    return [pscustomobject][ordered]@{
        outcome = $Outcome
        entry = $Entry
        field = $Field
        expected = $Expected
        rust = $Rust
        legacy = $Legacy
        message = $Message
    }
}

function Get-ComparisonOutcome {
    param([object[]]$Findings)

    foreach ($finding in @($Findings)) {
        if ($finding.outcome -eq "failure") {
            return "failure"
        }
    }

    foreach ($finding in @($Findings)) {
        if ($finding.outcome -eq "expected improvement/divergence") {
            return "expected improvement/divergence"
        }
    }

    return "match"
}

function Compare-NormalizedDiscovery {
    param(
        [object[]]$Rust,
        [object[]]$Legacy,
        $Fixture
    )

    $expected = @(Get-ExpectedDiscoveryEntries -Fixture $Fixture)
    $expectedByKey = @{}
    foreach ($entry in @($expected)) {
        $expectedByKey["$($entry.type)|$($entry.name)"] = $entry
    }

    $rustByKey = @{}
    foreach ($entry in @($Rust)) {
        $rustByKey["$($entry.type)|$($entry.name)"] = $entry
    }

    $legacyByKey = @{}
    foreach ($entry in @($Legacy)) {
        $legacyByKey["$($entry.type)|$($entry.name)"] = $entry
    }

    $allKeys = @($expectedByKey.Keys + $rustByKey.Keys + $legacyByKey.Keys | Sort-Object -Unique)
    $findings = @()
    foreach ($key in $allKeys) {
        if (-not $expectedByKey.ContainsKey($key)) {
            $findings += New-ComparisonFinding `
                -Outcome "failure" `
                -Entry $key `
                -Field "entry" `
                -Expected $null `
                -Rust $(if ($rustByKey.ContainsKey($key)) { "present" } else { $null }) `
                -Legacy $(if ($legacyByKey.ContainsKey($key)) { "present" } else { $null }) `
                -Message "Discovery returned an entry outside the controlled fixture contract."
            continue
        }

        $expectedEntry = $expectedByKey[$key]
        if (-not $legacyByKey.ContainsKey($key)) {
            $findings += New-ComparisonFinding `
                -Outcome "failure" `
                -Entry $key `
                -Field "entry" `
                -Expected "present" `
                -Rust $(if ($rustByKey.ContainsKey($key)) { "present" } else { $null }) `
                -Legacy $null `
                -Message "Legacy discovery missed a controlled fixture entry."
        }

        if (-not $rustByKey.ContainsKey($key)) {
            $findings += New-ComparisonFinding `
                -Outcome "failure" `
                -Entry $key `
                -Field "entry" `
                -Expected "present" `
                -Rust $null `
                -Legacy $(if ($legacyByKey.ContainsKey($key)) { "present" } else { $null }) `
                -Message "Rust discovery missed a controlled fixture entry."
            continue
        }

        $rustEntry = $rustByKey[$key]
        $legacyEntry = $null
        if ($legacyByKey.ContainsKey($key)) {
            $legacyEntry = $legacyByKey[$key]
        }

        if ($expectedEntry.type -ne "file") {
            continue
        }

        if ((Convert-ToUInt64 $rustEntry.sizeLogical) -ne (Convert-ToUInt64 $expectedEntry.sizeLogical) -or
            ($null -ne $legacyEntry -and (Convert-ToUInt64 $legacyEntry.sizeLogical) -ne (Convert-ToUInt64 $expectedEntry.sizeLogical))) {
            $findings += New-ComparisonFinding `
                -Outcome "failure" `
                -Entry $key `
                -Field "sizeLogical" `
                -Expected $expectedEntry.sizeLogical `
                -Rust $rustEntry.sizeLogical `
                -Legacy $(if ($null -ne $legacyEntry) { $legacyEntry.sizeLogical } else { $null }) `
                -Message "Logical size is a strict cross-backend contract for controlled fixture files."
        }

        if ((Convert-ToUInt64 $rustEntry.sizePhysical) -ne (Convert-ToUInt64 $expectedEntry.sizePhysical)) {
            $findings += New-ComparisonFinding `
                -Outcome "failure" `
                -Entry $key `
                -Field "sizePhysical" `
                -Expected $expectedEntry.sizePhysical `
                -Rust $rustEntry.sizePhysical `
                -Legacy $(if ($null -ne $legacyEntry) { $legacyEntry.sizePhysical } else { $null }) `
                -Message "Rust physical size must match the OS-reported allocation for controlled fixture files."
        }
        elseif ($null -ne $legacyEntry -and (Convert-ToUInt64 $legacyEntry.sizePhysical) -ne (Convert-ToUInt64 $expectedEntry.sizePhysical)) {
            $findings += New-ComparisonFinding `
                -Outcome "expected improvement/divergence" `
                -Entry $key `
                -Field "sizePhysical" `
                -Expected $expectedEntry.sizePhysical `
                -Rust $rustEntry.sizePhysical `
                -Legacy $legacyEntry.sizePhysical `
                -Message "Legacy physical size differs from the OS-reported allocation; this is reference output during migration, not parity failure."
        }

        if ($expectedEntry.requiresFileIndex -and (Convert-ToUInt64 $rustEntry.index) -eq 0) {
            $findings += New-ComparisonFinding `
                -Outcome "failure" `
                -Entry $key `
                -Field "index" `
                -Expected "nonzero" `
                -Rust $rustEntry.index `
                -Legacy $(if ($null -ne $legacyEntry) { $legacyEntry.index } else { $null }) `
                -Message "Rust discovery did not provide a usable file identity for a controlled fixture file."
        }
        elseif ($null -ne $legacyEntry -and (Convert-ToUInt64 $legacyEntry.index) -ne (Convert-ToUInt64 $rustEntry.index)) {
            $findings += New-ComparisonFinding `
                -Outcome "expected improvement/divergence" `
                -Entry $key `
                -Field "index" `
                -Expected "nonzero Rust file identity" `
                -Rust $rustEntry.index `
                -Legacy $legacyEntry.index `
                -Message "Legacy file identity is missing or differs; this remains an expected legacy limitation during migration."
        }
    }

    if ([string]$Fixture.name -eq "hardlinks" -and $rustByKey.ContainsKey("file|linked.bin") -and $rustByKey.ContainsKey("file|original.bin")) {
        $linkedIndex = Convert-ToUInt64 $rustByKey["file|linked.bin"].index
        $originalIndex = Convert-ToUInt64 $rustByKey["file|original.bin"].index
        if ($linkedIndex -eq 0 -or $originalIndex -eq 0 -or $linkedIndex -ne $originalIndex) {
            $findings += New-ComparisonFinding `
                -Outcome "failure" `
                -Entry "file|linked.bin,file|original.bin" `
                -Field "index" `
                -Expected "same nonzero Rust file identity" `
                -Rust ("linked={0}; original={1}" -f $linkedIndex, $originalIndex) `
                -Legacy $null `
                -Message "Rust discovery must report the same file identity for both hardlink names."
        }
    }

    return [pscustomobject][ordered]@{
        outcome = Get-ComparisonOutcome -Findings $findings
        expected = $expected
        findings = $findings
    }
}

function Start-ScanHost {
    param(
        [string]$Engine,
        [string]$HostPath,
        [string]$PipeName,
        [string]$LogFile
    )

    $signature = Get-AuthenticodeSignature -LiteralPath $HostPath -ErrorAction SilentlyContinue
    $signatureStatus = if ($null -ne $signature) { [string]$signature.Status } else { "Unknown" }
    Write-Host ("Launching scan host engine={0} path='{1}' signature={2}" -f $Engine, $HostPath, $signatureStatus)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $HostPath
    $psi.Arguments = "--pipe {0} --log-file {1} --log-level verbose" -f `
        (Quote-ProcessArgument "\\.\pipe\$PipeName"), `
        (Quote-ProcessArgument $LogFile)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    if ($Engine -eq "legacy") {
        $psi.EnvironmentVariables["WINDIRSTAT_REMOTE_DISCOVERY_ENGINE"] = "legacy"
    }
    else {
        $psi.EnvironmentVariables.Remove("WINDIRSTAT_REMOTE_DISCOVERY_ENGINE")
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    try {
        if (-not $process.Start()) {
            throw "Failed to start scan host '$HostPath'."
        }
    }
    catch {
        throw ("Failed to start scan host '{0}'. Signature status={1}. If Windows App Control still blocks launch, rerun with the signed build output at the same path or use a policy-allowed environment." -f $HostPath, $signatureStatus)
    }

    return $process
}

function Read-UntilRequestTerminal {
    param(
        [System.IO.Stream]$Stream,
        [uint64]$RequestId
    )

    while ($true) {
        $message = Read-RpcJson -Stream $Stream
        if ($null -eq $message) {
            continue
        }

        if ([uint64]$message.requestId -ne $RequestId) {
            continue
        }

        if ($message.kind -eq "ScanCanceledEvent" -or $message.kind -eq "ScanCompletedEvent") {
            return
        }

        if ($message.kind -eq "ScanFailedEvent") {
            throw "Scan host reported failure for request $RequestId at '$($message.path)': $($message.message)"
        }
    }
}

function Invoke-DiscoveryRequest {
    param(
        [System.IO.Stream]$Stream,
        [uint64]$RequestId,
        [string]$RootPath
    )

    Write-RpcJson -Stream $Stream -Message ([pscustomobject][ordered]@{
        kind = "StartScanRequest"
        requestId = $RequestId
        rootPath = $RootPath
        followMountPoints = $false
        followSymbolicLinks = $false
        followJunctions = $false
    })

    $progress = $null
    while ($null -eq $progress) {
        $message = Read-RpcJson -Stream $Stream
        if ($null -eq $message) {
            continue
        }

        if ([uint64]$message.requestId -ne $RequestId) {
            continue
        }

        if ($message.kind -eq "ScanFailedEvent") {
            throw "Scan host reported failure for request $RequestId at '$($message.path)': $($message.message)"
        }

        if ($message.kind -eq "DirectoryProgressEvent" -and
            [System.StringComparer]::OrdinalIgnoreCase.Equals([string]$message.directoryPath, $RootPath)) {
            $progress = $message
        }
    }

    Write-RpcJson -Stream $Stream -Message ([pscustomobject][ordered]@{
        kind = "CancelScanRequest"
        requestId = $RequestId
        reason = 1
    })
    Read-UntilRequestTerminal -Stream $Stream -RequestId $RequestId

    return Normalize-DiscoveryProgress -Progress $progress
}

function Invoke-HostDiscoverySession {
    param(
        [string]$Engine,
        [string]$HostPath,
        [object[]]$Fixtures,
        [string]$WorkspaceRoot
    )

    $pipeName = "WinDirStat.HostDiscoverySmoke.{0}.{1}.{2}" -f $Engine, $PID, ([Guid]::NewGuid().ToString("N"))
    $logFile = Join-Path $WorkspaceRoot ("host-{0}.log" -f $Engine)
    $process = $null
    $stream = $null
    $results = @{}

    try {
        $process = Start-ScanHost -Engine $Engine -HostPath $HostPath -PipeName $pipeName -LogFile $logFile
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

        $requestId = [uint64]1
        foreach ($fixture in $Fixtures) {
            $results[[string]$fixture.name] = Invoke-DiscoveryRequest `
                -Stream $stream `
                -RequestId $requestId `
                -RootPath ([string]$fixture.path)
            $requestId++
        }
    }
    finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }

        if ($null -ne $process) {
            if (-not $process.WaitForExit(2000)) {
                $process.Kill()
                $process.WaitForExit()
            }
            $process.Dispose()
        }
    }

    return [pscustomobject][ordered]@{
        engine = $Engine
        logFile = $logFile
        results = $results
    }
}

$resolvedHostPath = Resolve-SmokeHostPath -Path $HostPath
$fixtureRoot = New-SmokeTempRoot
$exitCode = 0

try {
    $fixtures = @(New-DiscoverySmokeFixtures -Root $fixtureRoot)
    $rustSession = Invoke-HostDiscoverySession -Engine "rust" -HostPath $resolvedHostPath -Fixtures $fixtures -WorkspaceRoot $fixtureRoot
    $legacySession = Invoke-HostDiscoverySession -Engine "legacy" -HostPath $resolvedHostPath -Fixtures $fixtures -WorkspaceRoot $fixtureRoot

    $caseResults = @()
    foreach ($fixture in $fixtures) {
        $rustEntries = @($rustSession.results[[string]$fixture.name])
        $legacyEntries = @($legacySession.results[[string]$fixture.name])
        $comparison = Compare-NormalizedDiscovery -Rust $rustEntries -Legacy $legacyEntries -Fixture $fixture
        if ($comparison.outcome -eq "failure") {
            $exitCode = 1
        }

        $caseResults += [pscustomobject][ordered]@{
            name = $fixture.name
            path = $fixture.path
            description = $fixture.description
            outcome = $comparison.outcome
            matches = ($comparison.outcome -eq "match")
            expected = $comparison.expected
            findings = $comparison.findings
            rust = $rustEntries
            legacy = $legacyEntries
        }
    }

    $summaryOutcome = Get-ComparisonOutcome -Findings @($caseResults | ForEach-Object { $_.findings })

    $summary = [pscustomobject][ordered]@{
        generatedAt = (Get-Date).ToUniversalTime().ToString("o")
        hostPath = $resolvedHostPath
        fixtureRoot = $fixtureRoot
        rustLogFile = $rustSession.logFile
        legacyLogFile = $legacySession.logFile
        fixturesPreserved = ($KeepFixtures.IsPresent -or $exitCode -ne 0)
        outcome = $summaryOutcome
        matches = ($summaryOutcome -eq "match")
        hasFailures = ($exitCode -ne 0)
        cases = $caseResults
    }

    $json = $summary | ConvertTo-Json -Depth 30
    if ($OutputPath -ne "") {
        $json | Set-Content -LiteralPath $OutputPath -Encoding UTF8
    }

    $json
}
finally {
    if (-not $KeepFixtures -and $exitCode -eq 0) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

exit $exitCode
