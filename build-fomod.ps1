param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$CoreBuildScript = Join-Path $Root "build-vortex.ps1"
$CorePackage = Join-Path $Root "package"
$OptionalPackage = Join-Path $Root "optional\OCumIntegration\package"
$UIConsentPackage = Join-Path $Root "compat\OStimUIConsent\package"
$FomodSource = Join-Path $Root "fomod"
$Stage = Join-Path $Root "release-fomod"
$Dist = Join-Path $Root "dist"
$VersionFile = Join-Path $Root "VERSION"

if (-not (Test-Path $VersionFile)) {
    throw "VERSION introuvable: $VersionFile"
}

$Version = (Get-Content $VersionFile -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION invalide: '$Version'"
}

$Zip = Join-Path $Dist "OStimTogether-v$Version-FOMOD.zip"

New-Item -ItemType Directory -Force -Path $Dist | Out-Null

& $CoreBuildScript -VcpkgRoot $VcpkgRoot -Configuration $Configuration

$OptionalEsp = Join-Path $OptionalPackage "Data\OStimTogether_OCum.esp"
$OptionalPex = Join-Path $OptionalPackage "Data\Scripts\OStimTogetherOCum.pex"

if (-not (Test-Path $OptionalEsp)) {
    throw "ESP OCum optionnel manquant: $OptionalEsp`nVoir optional\OCumIntegration\CreationKit-ESP.md"
}

if (-not (Test-Path $OptionalPex)) {
    throw "PEX OCum optionnel manquant: $OptionalPex`nCompile Data\Scripts\Source\OStimTogetherOCum.psc puis copie le PEX dans optional\OCumIntegration\package\Data\Scripts."
}

$UIConsentOSKSE = Join-Path $UIConsentPackage "Data\Scripts\OSKSE.pex"
$UIConsentNative = Join-Path $UIConsentPackage "Data\Scripts\OStimTogetherNative.pex"
if (-not (Test-Path $UIConsentOSKSE) -or -not (Test-Path $UIConsentNative)) {
    throw "Patch Papyrus Add Actor obligatoire non compile.`nExecute compat\OStimUIConsent\compile-ui-consent.ps1 puis relance build-fomod.ps1."
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
# Add Actor consent is core functionality. Overlay the compiled OSKSE.pex and
# OStimTogetherNative.pex directly into 00 Core so the FOMOD cannot omit it.
Copy-Item (Join-Path $UIConsentPackage "*") $CoreStage -Recurse -Force
Copy-Item (Join-Path $OptionalPackage "*") $OCumStage -Recurse -Force
Copy-Item (Join-Path $FomodSource "*") $FomodStage -Recurse -Force

$StagedInfo = Join-Path $FomodStage "info.xml"
if (Test-Path $StagedInfo) {
    $InfoContent = Get-Content $StagedInfo -Raw
    $InfoContent = [regex]::Replace(
        $InfoContent,
        '<Version>[^<]*</Version>',
        "<Version>$Version</Version>")
    Set-Content -Path $StagedInfo -Value $InfoContent -Encoding UTF8
}

if (Test-Path $Zip) {
    Remove-Item $Zip -Force
}

Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Zip -Force

Write-Host ""
Write-Host "OK - OStim Together v$Version FOMOD STRPM cree :" -ForegroundColor Green
Write-Host $Zip
Write-Host "Add Actor consent gate inclus obligatoirement dans 00 Core." -ForegroundColor Green
