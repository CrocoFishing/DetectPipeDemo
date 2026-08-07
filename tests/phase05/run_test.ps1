param([string]$DeviceName='ESP32S3-FACE-DEMO',[int]$Repeat=10)
$python = "$PSScriptRoot\..\..\pc_client\.venv\Scripts\python.exe"
Push-Location "$PSScriptRoot\..\..\pc_client"
try { & $python -m pc_client ble-test --device-name $DeviceName --repeat $Repeat --csv "..\results\phase05\actual\ble_metrics.csv" } finally { Pop-Location }

