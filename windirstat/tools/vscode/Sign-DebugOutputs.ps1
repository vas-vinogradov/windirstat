param(
    [string[]]$Path = @(
        "build\WinDirStat_x64.exe",
        "build\windirstat-scan-host_x64.exe"
    )
)

# Intent: repair the common VS Code workflow case where a compile-only build
# has produced fresh binaries but intentionally skipped the project post-build
# signing step required by local Application Control policy.
Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
[xml]$project = Get-Content -LiteralPath (Join-Path $repoRoot "windirstat.vcxproj")
$ns = [System.Xml.XmlNamespaceManager]::new($project.NameTable)
$ns.AddNamespace("msb", "http://schemas.microsoft.com/developer/msbuild/2003")

$thumbprintNode = $project.SelectSingleNode("//msb:WinDirStatDevCodeSignThumbprint", $ns)
$signToolNode = $project.SelectSingleNode("//msb:WinDirStatSignToolPath", $ns)
if (-not $thumbprintNode -or -not $signToolNode) {
    throw "Could not find WinDirStat signing properties in windirstat.vcxproj."
}

$thumbprint = $thumbprintNode.InnerText.Trim()
$signTool = $signToolNode.InnerText.Trim()
if (-not (Test-Path -LiteralPath $signTool)) {
    throw "signtool.exe was not found at '$signTool'."
}

$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.Thumbprint -eq $thumbprint } |
    Select-Object -First 1
if (-not $cert) {
    throw "WinDirStat dev code-signing cert '$thumbprint' was not found in CurrentUser\My."
}

foreach ($relativePath in $Path) {
    $target = Resolve-Path (Join-Path $repoRoot $relativePath)
    & $signTool sign /fd SHA256 /sha1 $thumbprint $target
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
