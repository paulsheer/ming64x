param(
    [string]$ImageName = 'vcxsrv-built-release-x64-xkbfixed',
    [string]$OutputDir = $null
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptPath = $MyInvocation.MyCommand.Path
$repoRoot = Split-Path $scriptPath -Parent
if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot 'build-output'
}
$outputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

Write-Host "Checking Docker image: $ImageName"
& docker image inspect $ImageName | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Docker image '$ImageName' was not found. Build or restore the verified checkpoint image first."
}

$tempDir = Join-Path $env:TEMP 'vcxsrv-installer-repack'
if (Test-Path $tempDir) {
    Remove-Item -Recurse -Force $tempDir
}
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

$repackage = @'
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir is not set. vcvarsall.bat probably did not run.' }
if (-not $env:OUTDIR) { throw 'OUTDIR is not set.' }

$installerDir = 'C:\vcx\xorg-server\installer'

if (-not (Test-Path 'C:\vcx\xorg-server\obj64\servrelease\vcxsrv.exe')) {
    throw 'Missing vcxsrv.exe in the verified image.'
}
if (-not (Test-Path 'C:\vcx\xorg-server\xkbdata\rules\xorg')) {
    throw 'Missing C:\vcx\xorg-server\xkbdata\rules\xorg in the verified image.'
}

New-Item -ItemType Directory -Force -Path $env:OUTDIR | Out-Null
Get-ChildItem -Path $installerDir -Filter 'vcxsrv-64.*.installer*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

$redistRoot = $env:VCToolsRedistDir
Copy-Item -Force (Join-Path $redistRoot 'x64\Microsoft.VC143.CRT\msvcp140.dll') $installerDir
Copy-Item -Force (Join-Path $redistRoot 'x64\Microsoft.VC143.CRT\vcruntime140.dll') $installerDir
Copy-Item -Force (Join-Path $redistRoot 'x64\Microsoft.VC143.CRT\vcruntime140_1.dll') $installerDir

$makensis = @(
    'C:\Program Files (x86)\NSIS\makensis.exe',
    'C:\Program Files\NSIS\makensis.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $makensis) {
    throw 'makensis.exe was not found in the image.'
}

Push-Location $installerDir
try {
    & $makensis 'vcxsrv-64.nsi'
    if ($LASTEXITCODE -ne 0) { throw 'NSIS packaging failed.' }
}
finally {
    Pop-Location
}

$installer = Get-ChildItem -Path $installerDir -Filter 'vcxsrv-64.*.installer*.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $installer) { throw 'Installer was not produced.' }

Copy-Item -Force $installer.FullName $env:OUTDIR
Copy-Item -Force 'C:\vcx\xorg-server\obj64\servrelease\vcxsrv.exe' (Join-Path $env:OUTDIR 'vcxsrv-release-x64.exe')
'@

$repackagePath = Join-Path $tempDir 'repackage.ps1'
Set-Content -LiteralPath $repackagePath -Value $repackage -Encoding ASCII

$containerArgs = @(
    'run',
    '--rm',
    '--entrypoint', 'cmd',
    '-e', 'OUTDIR=C:\out',
    '-v', ('{0}:C:\out' -f $outputDir),
    '-v', ('{0}:C:\build' -f $tempDir),
    $ImageName,
    '/S', '/C',
    'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64 10.0.19041.0 && powershell -NoProfile -ExecutionPolicy Bypass -File C:\build\repackage.ps1'
)

Write-Host "Packaging the verified build image..."
& docker @containerArgs
if ($LASTEXITCODE -ne 0) {
    throw 'Docker packaging failed.'
}

Get-ChildItem -Path $outputDir | Select-Object Name,Length,LastWriteTime
