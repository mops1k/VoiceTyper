# Builds the native VoiceTyper installer (Inno Setup).
#
#   pwsh installer/build-native-installer.ps1 [-Version 1.2.0] [-Preset windows-mingw-release]
#
# What it does: builds the deploy tree (the runnable application with its DLLs), finds
# the product version if one was not given, and runs Inno Setup on installer-native.iss.
# The result is VoiceTyper-<version>-win64-Setup.exe next to the deploy tree - the asset
# name the application's updater looks for.
#
# The "win64" in that name is not decoration: src/app/windows_application.cpp refuses to
# run any installer whose name lacks it, so the old .NET installer can never be applied
# over the native build.

param(
    [string]$Version = "",
    [string]$Preset = "windows-mingw-release"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ([string]::IsNullOrWhiteSpace($Version)) {
    $match = Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'set\(VOICETYPER_VERSION "([^"]+)"'
    if (-not $match) {
        throw 'the product version is not set in CMakeLists.txt and none was passed'
    }
    $Version = $match.Matches[0].Groups[1].Value
}
Write-Host "native installer: VoiceTyper $Version"

$iscc = @(
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    $fromPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($fromPath) { $iscc = $fromPath.Source }
}
if (-not $iscc) {
    Write-Error @'
Inno Setup 6 is not installed, so the installer cannot be built.
Install it with:  winget install -e --id JRSoftware.InnoSetup
(the GitHub release workflow installs it automatically; CI is the other way to get the asset)
'@
}

Write-Host 'building the deploy tree...'
& cmake --build --preset $Preset --target voicetyper-deploy
if ($LASTEXITCODE -ne 0) {
    throw "build failed with exit code $LASTEXITCODE"
}

$source = Join-Path $root "build\$Preset\deploy"
$output = Join-Path $root "build\$Preset"
if (-not (Test-Path (Join-Path $source 'VoiceTyper.exe')) -and -not (Test-Path (Join-Path $source 'voicetyper-qt-shell.exe'))) {
    throw "the deploy tree has no application executable: $source"
}

Write-Host "running Inno Setup: $iscc"
& $iscc "/DAppVersion=$Version" "/DSourceDir=$source" "/DOutputDir=$output" (Join-Path $PSScriptRoot 'installer-native.iss')
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup failed with exit code $LASTEXITCODE"
}

$installer = Join-Path $output "VoiceTyper-$Version-win64-Setup.exe"
if (-not (Test-Path $installer)) {
    throw "Inno Setup reported success but $installer is missing"
}
$size = [math]::Round((Get-Item $installer).Length / 1MB, 1)
Write-Host "release asset: $installer ($size MB)"
