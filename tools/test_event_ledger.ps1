param([string] $VcVars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    if (-not (Test-Path -LiteralPath $VcVars)) { throw "MSVC environment unavailable: $VcVars" }
    New-Item -ItemType Directory -Path tests/event1/build -Force | Out-Null
    # Fixed compilation targets; no filesystem mutation is composed across shells.
    $compileCommand = '"' + $VcVars + '" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /Ifirmware\components\pipeline_core\include /Ifirmware\components\event_manager\include tests\event1\event_ledger_test.cpp /Fetests\event1\build\event_ledger_test.exe /Fotests\event1\build\event_ledger_test.obj && tests\event1\build\event_ledger_test.exe'
    & cmd.exe /c $compileCommand
    if ($LASTEXITCODE -ne 0) { throw "Event ledger test failed: $LASTEXITCODE" }
    Write-Output 'PASS: actual C++ Event ledger, 200 events, completion orders, reboot sessions, stale results and failure ownership'
} finally { Pop-Location }
