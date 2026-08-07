param([string]$Port='COM5')
& "$PSScriptRoot\..\..\tools\idf.ps1" -App phase01_camera build
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$PSScriptRoot\..\..\tools\idf.ps1" -App phase01_camera -p $Port flash monitor

