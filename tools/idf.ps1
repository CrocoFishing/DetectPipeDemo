param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('phase01_camera','phase02_trigger','phase03_face_pipeline','phase04_protocol','phase05_ble','phase06_end_to_end')]
    [string] $App,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $IdfArgs
)
$ErrorActionPreference = 'Stop'
$profilePath = 'C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1'
if (-not (Test-Path -LiteralPath $profilePath)) { throw "ESP-IDF 6.0 profile not found: $profilePath" }
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. $profilePath
$env:GIT_CONFIG_COUNT = '1'
$env:GIT_CONFIG_KEY_0 = 'safe.directory'
$env:GIT_CONFIG_VALUE_0 = 'C:/esp/v6.0/esp-idf'
$projectRoot = Split-Path -Parent $PSScriptRoot
$appPath = Join-Path $projectRoot "firmware\apps\$App"
if (-not (Test-Path -LiteralPath $appPath)) { throw "Unknown firmware app path: $appPath" }
Push-Location $appPath
$ninjaPath = 'C:/Espressif/tools/ninja/1.12.1/ninja.exe'
try { & idf.py "-DCMAKE_MAKE_PROGRAM=$ninjaPath" @IdfArgs; exit $LASTEXITCODE } finally { Pop-Location }
