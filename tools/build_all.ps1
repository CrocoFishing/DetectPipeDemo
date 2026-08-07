param([switch] $Clean)
$ErrorActionPreference = 'Stop'
$apps = @('phase01_camera','phase02_trigger','phase03_face_pipeline','phase04_protocol','phase05_ble','phase06_end_to_end')
foreach ($app in $apps) {
    if ($Clean) { & "$PSScriptRoot\idf.ps1" -App $app fullclean; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
    & "$PSScriptRoot\idf.ps1" -App $app build
    if ($LASTEXITCODE) { throw "Build failed: $app" }
}

