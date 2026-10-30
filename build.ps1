#Requires -Version 5.1
<#
.SYNOPSIS
    Builds dist\rack.dll (x64, Release).
.EXAMPLE
    pwsh -File build.ps1
    pwsh -File build.ps1 -Clean -Package
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Package,
    [switch]$BuildHostSim
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root 'src'
$inc = Join-Path $root 'include'
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'

if ($Clean) {
    foreach ($path in @($build, $dist)) {
        if (Test-Path $path) { Remove-Item $path -Recurse -Force }
    }
}
foreach ($path in @($build, $dist)) {
    New-Item -ItemType Directory -Path $path -Force | Out-Null
}

function Find-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($found) { return $found.Trim() }
    }
    foreach ($candidate in @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
    )) {
        if (Test-Path (Join-Path $candidate 'VC\Auxiliary\Build\vcvars64.bat')) { return $candidate }
    }
    throw '未找到 Visual Studio C++ 工具链（需要 vcvars64.bat）。'
}

$vs = Find-VsInstall
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat 不存在: $vcvars" }

$sources = Get-ChildItem -Path $src -Filter *.cpp | Sort-Object Name | ForEach-Object { $_.FullName }
if (-not $sources) { throw 'src 下没有 .cpp 文件。' }

$rsp = Join-Path $build 'rack.rsp'
$lines = @(
    '/nologo'
    '/std:c++20'
    '/utf-8'
    '/EHsc'
    '/MT'
    '/O2'
    '/W4'
    '/permissive-'
    '/Zc:__cplusplus'
    '/D_CRT_SECURE_NO_WARNINGS'
    '/DWIN32_LEAN_AND_MEAN'
    '/D_WIN32_WINNT=0x0A00'
    '/DUNICODE'
    '/D_UNICODE'
    '/DSNOWAIR_PLUGIN_EXPORTS'
    "/I`"$inc`""
    "/I`"$src`""
    '/LD'
    "/Fe`"$dist\rack.dll`""
) + ($sources | ForEach-Object { "`"$_`"" }) + @(
    '/link'
    'd2d1.lib'
    'dwrite.lib'
    'windowscodecs.lib'
    'shlwapi.lib'
    'shell32.lib'
    'ole32.lib'
    'oleaut32.lib'
    'user32.lib'
    'gdi32.lib'
    'imm32.lib'
    'advapi32.lib'
    'comctl32.lib'
    'comdlg32.lib'
    'shcore.lib'
    'uuid.lib'
)
Set-Content -Path $rsp -Value $lines -Encoding ascii

$rc = Join-Path $src 'rack.rc'
if (Test-Path $rc) {
    $rcLine = "call `"$vcvars`" >nul && rc /nologo /I `"$src`" /fo `"$build\rack.res`" `"$rc`""
    Write-Host '==> rc rack.rc' -ForegroundColor Cyan
    & cmd.exe /c $rcLine
    if ($LASTEXITCODE -ne 0) { throw "rc 失败 (exit $LASTEXITCODE)" }
    Add-Content -Path $rsp -Value "`"$build\rack.res`""
}

Write-Host "==> cl (x64 Release, $($sources.Count) sources)" -ForegroundColor Cyan
$line = "call `"$vcvars`" >nul && cd /d `"$build`" && cl @`"$rsp`""
& cmd.exe /c $line
if ($LASTEXITCODE -ne 0) { throw "cl 失败 (exit $LASTEXITCODE)" }

$dll = Join-Path $dist 'rack.dll'
if (-not (Test-Path $dll)) { throw "未生成 $dll" }
Set-Content -Path (Join-Path $dist '.build-ok') -Value (Get-Date -Format o) -Encoding ascii
$size = [math]::Round((Get-Item $dll).Length / 1KB, 1)
Write-Host "==> dist\rack.dll  ($size KB)" -ForegroundColor Green

$assetsSrc = Join-Path $root 'assets'
if (Test-Path $assetsSrc) {
    Copy-Item $assetsSrc $dist -Recurse -Force
    Write-Host '==> dist\assets' -ForegroundColor Green
}

if ($BuildHostSim) {
    $simSrc = Join-Path $root 'tests\HostSim\HostSim.cpp'
    $simRsp = Join-Path $build 'hostsim.rsp'
    $simLines = @(
        '/nologo'
        '/std:c++20'
        '/utf-8'
        '/EHsc'
        '/MT'
        '/O2'
        '/W4'
        '/permissive-'
        '/Zc:__cplusplus'
        '/D_CRT_SECURE_NO_WARNINGS'
        '/D_WIN32_WINNT=0x0A00'
        '/DUNICODE'
        '/D_UNICODE'
        "/I`"$inc`""
        "/Fe`"$dist\HostSim.exe`""
        "`"$simSrc`""
        '/link'
        'user32.lib'
        'gdi32.lib'
        'shell32.lib'
        'ole32.lib'
        'comdlg32.lib'
    )
    New-Item -ItemType Directory -Path (Join-Path $build 'hostsim') -Force | Out-Null
    Set-Content -Path $simRsp -Value $simLines -Encoding ascii
    Write-Host '==> HostSim' -ForegroundColor Cyan
    $line = "call `"$vcvars`" >nul && cd /d `"$(Join-Path $build 'hostsim')`" && cl @`"$simRsp`""
    & cmd.exe /c $line
    if ($LASTEXITCODE -ne 0) { throw "HostSim 构建失败 (exit $LASTEXITCODE)" }
}

if ($Package) {
    $versionLine = Select-String -Path (Join-Path $src 'Version.h') -Pattern 'SA_RACK_VERSION\s+L"([^"]+)"'
    $version = $versionLine.Matches[0].Groups[1].Value
    $stage = Join-Path $build 'package'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    $rackDir = Join-Path $stage 'rack'
    New-Item -ItemType Directory -Path $rackDir -Force | Out-Null
    Copy-Item $dll $rackDir
    foreach ($extra in @('README.md', 'LICENSE', 'DESIGN.md', 'THIRD_PARTY_NOTICES.md')) {
        $path = Join-Path $root $extra
        if (Test-Path $path) { Copy-Item $path $rackDir }
    }
    $docs = Join-Path $root 'docs'
    if (Test-Path $docs) { Copy-Item $docs $rackDir -Recurse }
    if (Test-Path $assetsSrc) { Copy-Item $assetsSrc $rackDir -Recurse }
    $zip = Join-Path $dist "SnowAir-Rack-v$version.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $rackDir -DestinationPath $zip
    Write-Host "==> $zip" -ForegroundColor Green
}
