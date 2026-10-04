param([switch] $LegacyOnly)
$ErrorActionPreference = 'Stop'
$targets = @('phase01_camera', 'phase02_trigger', 'phase03_face_pipeline', 'phase04_protocol', 'phase05_ble', 'phase06_end_to_end')
if (-not $LegacyOnly) { $targets += 'event1_audio_face' }
foreach ($target in $targets) {
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'idf.ps1') -App $target build
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $target ($LASTEXITCODE)" }
}
