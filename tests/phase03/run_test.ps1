param([string]$Port='COM5')
& "$PSScriptRoot\..\..\tools\idf.ps1" -App phase03_face_pipeline build
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$PSScriptRoot\..\..\tools\idf.ps1" -App phase03_face_pipeline -p $Port flash monitor

