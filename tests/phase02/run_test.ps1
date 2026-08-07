param([string]$Port='COM5')
& "$PSScriptRoot\..\..\tools\idf.ps1" -App phase02_trigger build
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$PSScriptRoot\..\..\tools\idf.ps1" -App phase02_trigger -p $Port flash monitor

