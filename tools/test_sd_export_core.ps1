param([string] $VcVars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    if (-not (Test-Path -LiteralPath $VcVars)) { throw "MSVC environment unavailable: $VcVars" }
    New-Item -ItemType Directory -Path tests/wifi_sd_export/build -Force | Out-Null
    $compileCommand = '"' + $VcVars + '" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /Ifirmware\components\wifi_sd_export\include tests\wifi_sd_export\core_test.cpp /Fetests\wifi_sd_export\build\core_test.exe /Fotests\wifi_sd_export\build\core_test.obj && tests\wifi_sd_export\build\core_test.exe'
    & cmd.exe /c $compileCommand
    if ($LASTEXITCODE -ne 0) { throw "SD export core test failed: $LASTEXITCODE" }
} finally { Pop-Location }
