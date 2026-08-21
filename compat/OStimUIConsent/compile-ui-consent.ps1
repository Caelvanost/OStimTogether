param(
    [string]$SkyrimDir = "C:\Games\Steam\steamapps\common\Skyrim Special Edition",
    [string]$OStimSourceDir = "",
    [string]$UIExtensionsSourceDir = ""
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$SourceDir = Join-Path $Root "Data\Scripts\Source"
$PackageScripts = Join-Path $Root "package\Data\Scripts"
$Compiler = Join-Path $SkyrimDir "Papyrus Compiler\PapyrusCompiler.exe"
$GameSources = Join-Path $SkyrimDir "Data\Source\Scripts"
$SKSESources = Join-Path $SkyrimDir "Data\Scripts\Source"
$Flags = Join-Path $GameSources "TESV_Papyrus_Flags.flg"

if (-not (Test-Path $Compiler)) { throw "PapyrusCompiler.exe introuvable: $Compiler" }
if (-not (Test-Path $Flags)) { throw "TESV_Papyrus_Flags.flg introuvable: $Flags" }
if (-not (Test-Path (Join-Path $SourceDir "OSKSE.psc"))) { throw "OSKSE.psc patch introuvable" }
if (-not (Test-Path (Join-Path $SourceDir "OStimTogetherNative.psc"))) { throw "OStimTogetherNative.psc introuvable" }

$Includes = @($SourceDir, $SKSESources, $GameSources)
if ($OStimSourceDir) { $Includes += $OStimSourceDir }
if ($UIExtensionsSourceDir) { $Includes += $UIExtensionsSourceDir }
$IncludeArg = $Includes -join ";"

New-Item -ItemType Directory -Force -Path $PackageScripts | Out-Null

foreach ($Source in @("OStimTogetherNative.psc", "OSKSE.psc")) {
    $SourceFile = Join-Path $SourceDir $Source
    & $Compiler $SourceFile "-f=$Flags" "-i=$IncludeArg" "-o=$PackageScripts"
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation Papyrus echouee pour $Source (code $LASTEXITCODE). Passe -OStimSourceDir vers les sources Papyrus d'OStim et -UIExtensionsSourceDir si necessaire."
    }
}

foreach ($Pex in @("OStimTogetherNative.pex", "OSKSE.pex")) {
    $Path = Join-Path $PackageScripts $Pex
    if (-not (Test-Path $Path)) { throw "PEX non produit: $Path" }
}

Write-Host "OK - OStim UI consent patch compile dans $PackageScripts" -ForegroundColor Green
Write-Host "Le fichier OSKSE.pex doit gagner le conflit face a OStim pour suspendre Add Actor avant consentement." -ForegroundColor Yellow
