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
    # Redirect SDK headers only; compile the complete production service against
    # controlled hardware boundaries to exercise shutdown failure and SD reads.
    $stubRoot = Join-Path $repoRoot 'tests/wifi_sd_export/build/network_stubs'
    $headers = @('esp_err.h','esp_event.h','esp_heap_caps.h','esp_http_server.h','esp_log.h',
        'esp_netif.h','esp_random.h','esp_timer.h','esp_wifi.h','esp_wifi_default.h',
        'freertos/FreeRTOS.h','freertos/queue.h','freertos/task.h','ff.h','lwip/sockets.h','psa/crypto.h')
    foreach ($header in $headers) {
        $target = Join-Path $stubRoot $header
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Set-Content -LiteralPath $target -Value '#include "network_shims.hpp"' -Encoding ascii
    }
    $networkCommand = '"' + $VcVars + '" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /Itests\wifi_sd_export\build\network_stubs /Itests\wifi_sd_export\network_host_stubs /Ifirmware\components\application_protocol\include /Ifirmware\components\wifi_sd_export\include tests\wifi_sd_export\network_lifecycle_test.cpp /Fetests\wifi_sd_export\build\network_lifecycle_test.exe /Fotests\wifi_sd_export\build\network_lifecycle_test.obj && tests\wifi_sd_export\build\network_lifecycle_test.exe'
    & cmd.exe /c $networkCommand
    if ($LASTEXITCODE -ne 0) { throw "SD export network lifecycle test failed: $LASTEXITCODE" }
} finally { Pop-Location }
