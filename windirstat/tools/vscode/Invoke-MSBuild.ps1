param(
    [Parameter(Mandatory = $true)]
    [string]$Project,

    [ValidateSet("Debug", "DebugVerbose", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32", "ARM64")]
    [string]$Platform = "x64",

    [ValidateSet("Build", "Rebuild", "Clean")]
    [string]$Target = "Build",

    [switch]$SkipPostBuildEvent
)

# Intent: give VS Code tasks a repeatable MSVC/MSBuild entry point without
# requiring the editor to be launched from a Visual Studio developer shell.
Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$projectPath = Resolve-Path (Join-Path $repoRoot $Project)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio Build Tools with the MSVC and Windows SDK workloads."
}

$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    throw "No Visual Studio Build Tools installation with MSVC x86/x64 tools was found."
}

$vsDevCmd = Join-Path $vsInstall "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "VsDevCmd.bat was not found at '$vsDevCmd'."
}

$platformArg = if ($Platform -eq "Win32") { "x86" } else { $Platform }
$msbuildArgs = @(
    "`"$projectPath`"",
    "/t:$Target",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/m",
    "/v:minimal"
)

if ($SkipPostBuildEvent) {
    $msbuildArgs += "/p:PostBuildEventUseInBuild=false"
}

$command = "call `"$vsDevCmd`" -arch=$platformArg -host_arch=x64 >nul && msbuild $($msbuildArgs -join ' ')"
cmd.exe /c $command
exit $LASTEXITCODE
