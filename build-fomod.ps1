param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$CoreBuildScript = Join-Path $Root "build-vortex.ps1"
$CorePackage = Join-Path $Root "package"
$OptionalPackage = Join-Path $Root "optional\OCumIntegration\package"
$FomodSource = Join-Path $Root "fomod"
$Stage = Join-Path $Root "release-fomod"
$Zip = Join-Path $Root "OStimTogether-v0.19.2-FOMOD.zip"

& $CoreBuildScript -VcpkgRoot $VcpkgRoot -Configuration $Configuration

$OptionalEsp = Join-Path $OptionalPackage "Data\OStimTogether_OCum.esp"
$OptionalPex = Join-Path $OptionalPackage "Data\Scripts\OStimTogetherOCum.pex"

if (-not (Test-Path $OptionalEsp)) {
    throw "ESP OCum optionnel manquant: $OptionalEsp`nVoir optional\OCumIntegration\CreationKit-ESP.md"
}

if (-not (Test-Path $OptionalPex)) {
    throw "PEX OCum optionnel manquant: $OptionalPex`nCompile Data\Scripts\Source\OStimTogetherOCum.psc puis copie le PEX dans optional\OCumIntegration\package\Data\Scripts."
}

Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Stage | Out-Null

$CoreStage = Join-Path $Stage "00 Core"
$OCumStage = Join-Path $Stage "10 OCum Ascended"
$FomodStage = Join-Path $Stage "fomod"

New-Item -ItemType Directory -Force -Path $CoreStage | Out-Null
New-Item -ItemType Directory -Force -Path $OCumStage | Out-Null
New-Item -ItemType Directory -Force -Path $FomodStage | Out-Null

Copy-Item (Join-Path $CorePackage "*") $CoreStage -Recurse -Force
Copy-Item (Join-Path $OptionalPackage "*") $OCumStage -Recurse -Force
Copy-Item (Join-Path $FomodSource "*") $FomodStage -Recurse -Force

if (Test-Path $Zip) {
    Remove-Item $Zip -Force
}

Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Zip -Force

Write-Host ""
Write-Host "OK - FOMOD 0.19.2 cree :" -ForegroundColor Green
Write-Host $Zip
