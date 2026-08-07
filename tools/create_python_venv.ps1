param([string] $Python = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$venv = Join-Path $root 'pc_client\.venv'
if ($Python) { & $Python -m venv $venv }
elseif (Get-Command py -ErrorAction SilentlyContinue) {
    $installed = (& py -0p 2>$null) -join "`n"
    if ($installed -match '-V:3\.11') { & py -3.11 -m venv $venv } else { & py -3 -m venv $venv }
} else { & python -m venv $venv }
$pythonExe = Join-Path $venv 'Scripts\python.exe'
& $pythonExe -m pip install --upgrade pip
if ($LASTEXITCODE) { throw "pip upgrade failed" }
& $pythonExe -m pip install -r (Join-Path $root 'pc_client\requirements.lock')
if ($LASTEXITCODE) { throw "dependency install failed" }
Write-Output "Use directly: $pythonExe"
