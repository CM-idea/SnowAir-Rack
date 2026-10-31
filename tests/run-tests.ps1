#Requires -Version 5.1
<#
.SYNOPSIS
    Smoke test for SnowAir-Rack. Builds the DLL + HostSim, checks the export and
    runs the test host with a few captures.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $root
$dist = Join-Path $repo 'dist'
$dll = Join-Path $dist 'rack.dll'
$sim = Join-Path $dist 'HostSim.exe'
$log = Join-Path $repo 'HostSim.log'

Push-Location $repo
try {
    & (Join-Path $repo 'build.ps1') -BuildHostSim
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed: $LASTEXITCODE" }
    if (!(Test-Path $dll)) { throw "missing $dll" }
    if (!(Test-Path $sim)) { throw "missing $sim" }

    # Verify the one required export with dumpbin from the VS toolchain.
    $dumpbin = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue |
        Select-Object -Last 1 -ExpandProperty FullName
    if ($dumpbin) {
        $exports = & $dumpbin /nologo /exports $dll
        if (($exports -join "`n") -notmatch 'SnowAirPluginEntry') {
            throw 'rack.dll does not export SnowAirPluginEntry'
        }
        Write-Host 'export: SnowAirPluginEntry OK' -ForegroundColor Green
    } else {
        Write-Warning 'dumpbin not found; skipping export check'
    }

    if (Test-Path $log) { Remove-Item $log -Force }
    # A persisted hanging history would make the same sample a duplicate.
    Remove-Item (Join-Path $dist 'data\rack_items.json') -Force -ErrorAction SilentlyContinue
    $sample = Join-Path $root 'assets\sample1.png'
    if (!(Test-Path $sample)) { throw "missing $sample" }

    $process = Start-Process -FilePath $sim `
        -ArgumentList @('--dll', $dll, '--image', $sample, '--seconds', '4') `
        -NoNewWindow -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "HostSim failed: $($process.ExitCode)" }

    $text = if (Test-Path $log) { Get-Content $log -Raw -Encoding Unicode } else { '' }
    if ($text -notmatch 'Capture added') {
        throw 'HostSim did not report a capture'
    }
    if ($text -notmatch '0\.1\.0') {
        throw 'HostSim log does not contain the plugin version'
    }
    Write-Host 'HostSim smoke test OK' -ForegroundColor Green
    Write-Host 'All checks passed.' -ForegroundColor Green
} finally {
    Pop-Location
}
