#Requires -Version 5.1
# Build SDL3 with the DualSense patches and put it into libs\windows\lib\x64,
# replacing the stock SDL3 from setup-deps.ps1. Needs git, CMake and Visual Studio 2022 (C++).
$ErrorActionPreference = 'Stop'

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SdlTag = 'release-3.4.18'  # same SDL as moonlight-qt-deps v19 (setup-deps.ps1)
$Src = Join-Path $Repo "build\SDL-$SdlTag"
$Lib = Join-Path $Repo 'libs\windows\lib\x64'

if (-not (Test-Path -LiteralPath $Lib)) { throw "missing $Lib. Run setup-deps.ps1 first." }

function Assert-Ok([string]$What) { if ($LASTEXITCODE) { throw "$What failed" } }

# Clone and patch into a temp dir so a half-done checkout is never reused.
if (-not (Test-Path -LiteralPath $Src)) {
    $tmp = "$Src.tmp"
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    git clone -q --depth 1 --branch $SdlTag https://github.com/libsdl-org/SDL $tmp; Assert-Ok 'git clone'
    $patches = @(Get-ChildItem (Join-Path $Repo 'sdl-patches\*.patch') | Sort-Object Name | ForEach-Object { $_.FullName })
    git -C $tmp apply @patches; Assert-Ok 'git apply'
    Rename-Item -LiteralPath $tmp -NewName (Split-Path -Leaf $Src)
}
cmake -S $Src -B "$Src\build" -G 'Visual Studio 17 2022' -A x64 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
Assert-Ok 'cmake configure'
cmake --build "$Src\build" --config RelWithDebInfo; Assert-Ok 'cmake build'

foreach ($f in 'SDL3.dll', 'SDL3.lib', 'SDL3.pdb') {
    Copy-Item -LiteralPath (Join-Path "$Src\build\RelWithDebInfo" $f) -Destination $Lib -Force
}
Write-Host "Patched SDL3 copied to $Lib"
