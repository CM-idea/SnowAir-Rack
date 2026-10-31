param(
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Get-Process HostSim -ErrorAction SilentlyContinue | Stop-Process -Force

$dll = Join-Path $root 'dist\rack.dll'
$sim = Join-Path $root 'dist\HostSim.exe'
$marker = Join-Path $root 'dist\.build-ok'
$assets = Join-Path $root 'dist\assets\basket.png'

$needBuild = $Rebuild -or
    -not (Test-Path $dll) -or
    -not (Test-Path $sim) -or
    -not (Test-Path $marker) -or
    -not (Test-Path $assets)

if (-not $needBuild) {
    $stamp = (Get-Item $marker).LastWriteTimeUtc
    $newest = Get-ChildItem -Path (Join-Path $root 'src'), (Join-Path $root 'include') -Recurse -File |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($newest -and $newest.LastWriteTimeUtc -gt $stamp) {
        Write-Host '源码比当前 rack.dll 新，需要重新构建。' -ForegroundColor Yellow
        $needBuild = $true
    }
}

if ($needBuild) {
    Write-Host '正在构建 rack.dll 和 HostSim...' -ForegroundColor Cyan
    try {
        & (Join-Path $root 'build.ps1') -BuildHostSim
    } catch {
        Write-Host ''
        Write-Host '构建失败：' -ForegroundColor Red
        Write-Host $_.Exception.Message -ForegroundColor Red
        exit 1
    }
}

if (-not (Test-Path $dll) -or -not (Test-Path $sim)) {
    Write-Host '缺少 dist\rack.dll 或 dist\HostSim.exe。' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host 'SnowAir 晾衣架 - 本地预览' -ForegroundColor Cyan
Write-Host '  - 按 Esc 退出'
Write-Host '  - 120 秒后自动退出'
Write-Host ''

& $sim `
    --image (Join-Path $root 'tests\assets\sample1.png') `
    --image (Join-Path $root 'tests\assets\sample2.png') `
    --image (Join-Path $root 'tests\assets\sample3.png') `
    --all --seconds 120

$exitCode = $LASTEXITCODE
Write-Host ''
Write-Host '预览结束。' -ForegroundColor Green
if ($exitCode -ne 0) {
    exit $exitCode
}
