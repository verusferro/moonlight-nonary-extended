#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

# Packaging helpers called by build-arch.bat.
#   check-sdl <SDL3.dll>             refuse the stock moonlight-qt-deps v19 SDL3.dll
#   licenses  <repo> [deploy-dir]    copy license texts into <deploy>\resources\licenses

$StockSdlSha256 = 'adb5717f9159c68f48ee7fed86d5d744eca0857b3a20a085794d193cf4335ae8'

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "missing file: $Path" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-PatchedSdl([string]$Path) {
    if ((Get-Sha256 $Path) -eq $StockSdlSha256) {
        throw "$Path is the stock SDL3.dll. Run scripts\build-sdl-windows.ps1 first."
    }
}

function Copy-LicenseNoClobber([string]$Src, [string]$Dest) {
    if (-not (Test-Path -LiteralPath $Src)) { throw "missing required license source: $Src" }
    $dir = Split-Path -Parent $Dest
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (-not (Test-Path -LiteralPath $Dest)) { Copy-Item -LiteralPath $Src -Destination $Dest }
}

function Invoke-CopyPackageLicenses([string]$RepoRoot, [string]$DeployDir) {
    if (-not $DeployDir) { $DeployDir = Join-Path $RepoRoot 'build\deploy-x64-release' }
    if (-not (Test-Path -LiteralPath $DeployDir)) { throw "missing deploy dir for licenses: $DeployDir" }
    $licRoot = Join-Path $DeployDir 'resources\licenses'
    New-Item -ItemType Directory -Force -Path $licRoot | Out-Null
    Copy-Item -Path (Join-Path $RepoRoot 'packaging\licenses\*') -Destination $licRoot -Recurse -Force

    $inTree = @(
        @('LICENSE', 'Moonlight\LICENSE'),
        @('moonlight-common-c\moonlight-common-c\LICENSE.txt', 'moonlight-common-c\LICENSE.txt'),
        @('qmdnsengine\qmdnsengine\LICENSE.txt', 'qmdnsengine\LICENSE.txt'),
        @('h264bitstream\LICENSE', 'h264bitstream\LICENSE'),
        @('app\SDL_GameControllerDB\LICENSE', 'SDL_GameControllerDB\LICENSE'),
        @('moonlight-common-c\moonlight-common-c\enet\LICENSE', 'enet\LICENSE'),
        @('moonlight-common-c\moonlight-common-c\nanors\LICENSE', 'nanors\LICENSE'),
        @('pyrowave\pyrowave\LICENSE', 'pyrowave\LICENSE'),
        @('pyrowave\pyrowave\Granite\LICENSE', 'Granite\LICENSE'),
        @('pyrowave\pyrowave\Granite\third_party\volk\LICENSE.md', 'volk\LICENSE.md'),
        @('third-party\saxense\LICENSE', 'saxense\LICENSE')
    )
    foreach ($pair in $inTree) {
        Copy-LicenseNoClobber (Join-Path $RepoRoot $pair[0]) (Join-Path $licRoot $pair[1])
    }
    Write-Host "Copied package licenses into $licRoot"
}

$action = $args[0]
switch ($action) {
    'check-sdl' { Assert-PatchedSdl $args[1] }
    'licenses' { Invoke-CopyPackageLicenses $args[1] $args[2] }
    default { throw "usage: port-build.ps1 check-sdl <SDL3.dll> | licenses <repo> [deploy-dir]" }
}
