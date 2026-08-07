param([string]$DeviceName='ESP32S3-FACE-DEMO')
$python = "$PSScriptRoot\..\..\pc_client\.venv\Scripts\python.exe"
Push-Location "$PSScriptRoot\..\..\pc_client"
try { & $python -m pc_client run --device-name $DeviceName } finally { Pop-Location }

