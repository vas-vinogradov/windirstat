param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32", "ARM64")]
    [string]$Platform = "x64"
)

# Intent: derive IntelliSense commands from the MSBuild projects and the real
# MSVC environment, keeping compile_commands.json local to each developer box.
Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio Build Tools with the MSVC and Windows SDK workloads."
}

$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    throw "No Visual Studio Build Tools installation with MSVC x86/x64 tools was found."
}

$vsDevCmd = Join-Path $vsInstall "Common7\Tools\VsDevCmd.bat"
$platformArg = if ($Platform -eq "Win32") { "x86" } else { $Platform }
$envCommand = "call `"$vsDevCmd`" -arch=$platformArg -host_arch=x64 >nul && set"
$envDump = cmd.exe /c $envCommand
$envMap = @{}
foreach ($line in $envDump) {
    $separator = $line.IndexOf("=")
    if ($separator -gt 0) {
        $envMap[$line.Substring(0, $separator)] = $line.Substring($separator + 1)
    }
}

$clPath = ($envMap["Path"] -split ";" | Where-Object { $_ } | ForEach-Object { Join-Path $_ "cl.exe" } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)
if (-not $clPath) {
    throw "cl.exe was not found after initializing the Visual Studio developer environment."
}

function Split-MsBuildList {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return @()
    }

    return @($Value -split ";" | Where-Object {
        $_ -and $_ -notlike "%(*"
    })
}

function Expand-ProjectValue {
    param(
        [string]$Value,
        [string]$ProjectDir,
        [string]$ProjectName
    )

    $solutionDir = (Resolve-Path (Join-Path $repoRoot "..")).ProviderPath + "\"
    $intDir = Join-Path $solutionDir "intermediate\$($Platform)_$($Configuration)\$ProjectName\"
    $expanded = $Value
    $expanded = $expanded.Replace('$(SolutionDir)', $solutionDir)
    $expanded = $expanded.Replace('$(ProjectDir)', $ProjectDir)
    $expanded = $expanded.Replace('$(IntDir)', $intDir)
    $expanded = $expanded.Replace('$(Configuration)', $Configuration)
    $expanded = $expanded.Replace('$(Platform)', $Platform)
    if ([System.IO.Path]::IsPathRooted($expanded)) {
        return [System.IO.Path]::GetFullPath($expanded)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $expanded))
}

function Get-XmlText {
    param(
        [xml]$Xml,
        [System.Xml.XmlNamespaceManager]$Ns,
        [string]$XPath
    )

    $node = $Xml.SelectSingleNode($XPath, $Ns)
    if ($node) {
        return $node.InnerText
    }

    return ""
}

function New-ProjectCompileCommands {
    param([string]$ProjectFile)

    $projectPath = Resolve-Path (Join-Path $repoRoot $ProjectFile)
    $projectDir = Split-Path -Parent $projectPath
    $projectDirWithSlash = $projectDir.TrimEnd("\") + "\"
    $projectName = [System.IO.Path]::GetFileNameWithoutExtension($projectPath)

    [xml]$xml = Get-Content -LiteralPath $projectPath
    $ns = [System.Xml.XmlNamespaceManager]::new($xml.NameTable)
    $ns.AddNamespace("msb", "http://schemas.microsoft.com/developer/msbuild/2003")

    $condition = "'`$(Configuration)|`$(Platform)'=='$Configuration|$Platform'"
    $baseIncludeText = Get-XmlText $xml $ns "//msb:ItemDefinitionGroup[@Condition=`"$condition`"]/msb:ClCompile/msb:AdditionalIncludeDirectories"
    $baseDefineText = Get-XmlText $xml $ns "//msb:ItemDefinitionGroup[@Condition=`"$condition`"]/msb:ClCompile/msb:PreprocessorDefinitions"

    $includeDirs = New-Object System.Collections.Generic.List[string]
    foreach ($include in Split-MsBuildList $baseIncludeText) {
        $includeDirs.Add((Expand-ProjectValue $include $projectDirWithSlash $projectName))
    }

    if ($envMap.ContainsKey("INCLUDE")) {
        foreach ($include in ($envMap["INCLUDE"] -split ";" | Where-Object { $_ })) {
            $includeDirs.Add($include)
        }
    }

    $defines = New-Object System.Collections.Generic.List[string]
    foreach ($define in Split-MsBuildList $baseDefineText) {
        $defines.Add($define)
    }
    $defines.Add("UNICODE")
    $defines.Add("_UNICODE")

    $compileNodes = $xml.SelectNodes("//msb:ClCompile[@Include]", $ns)
    foreach ($node in $compileNodes) {
        $relativeSource = $node.GetAttribute("Include")
        $sourcePath = [System.IO.Path]::GetFullPath((Join-Path $projectDir $relativeSource))
        if (-not (Test-Path -LiteralPath $sourcePath)) {
            continue
        }

        $args = New-Object System.Collections.Generic.List[string]
        $args.Add("/nologo")
        $args.Add("/TP")
        $args.Add("/std:c++20")
        $args.Add("/utf-8")
        $args.Add("/EHsc")
        $args.Add("/Zc:__cplusplus")
        $args.Add("/D_CRT_SECURE_NO_WARNINGS")
        foreach ($define in $defines | Select-Object -Unique) {
            $args.Add("/D$define")
        }
        foreach ($include in $includeDirs | Select-Object -Unique) {
            $args.Add("/I$include")
        }
        if ([System.IO.Path]::GetFileName($sourcePath) -ne "pch.cpp") {
            $args.Add("/FI$projectDirWithSlash`pch.h")
        }
        $args.Add("/c")
        $args.Add($sourcePath)

        [pscustomobject]@{
            directory = $projectDir
            file = $sourcePath
            arguments = @($clPath) + @($args)
        }
    }
}

$commands = @()
$commands += New-ProjectCompileCommands "windirstat.vcxproj"
$commands += New-ProjectCompileCommands "windirstat-scan-host.vcxproj"

$outputPath = Join-Path $repoRoot "compile_commands.json"
$commands | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputPath -Encoding UTF8
Write-Host "Wrote $($commands.Count) compile commands to $outputPath"
