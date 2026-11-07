$ErrorActionPreference = 'Stop'
$testPath = $PSScriptRoot
$repository = Split-Path -Parent (Split-Path -Parent $testPath)
$build = Join-Path $testPath 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$compilerSetup = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $compilerSetup)) { throw "Visual C++ setup unavailable: $compilerSetup" }
$source = Join-Path $testPath 'audio_artifact_test.c'
$stubs = Join-Path $testPath 'host_stubs'
$common = Join-Path $repository 'firmware\components\audio_common\include'
$ring = Join-Path $repository 'firmware\components\ring_buffer\include'
$wav = Join-Path $repository 'firmware\components\recorder\include'
$recorderSource = Join-Path $testPath 'recorder_init_test.c'
<<<<<<< HEAD
$recorderQueueSource = Join-Path $testPath 'recorder_queue_test.c'
=======
>>>>>>> fad0770f9a6e5c0128515521f1e160a23e27bf04
$recorderStubs = Join-Path $testPath 'recorder_host_stubs'
$recorderPrivate = Join-Path $repository 'firmware\components\recorder\private_include'
$afeSource = Join-Path $testPath 'afe_init_test.c'
$afeStubs = Join-Path $testPath 'afe_host_stubs'
$dsp = Join-Path $repository 'firmware\components\audio_dsp\include'
$dspPrivate = Join-Path $repository 'firmware\components\audio_dsp\private_include'
$pipelineSource = Join-Path $testPath 'pipeline_init_test.c'
$pipelineStubs = Join-Path $testPath 'pipeline_host_stubs'
$pipeline = Join-Path $repository 'firmware\components\audio_pipeline\include'
$capture = Join-Path $repository 'firmware\components\audio_capture\include'
$captureSource = Join-Path $testPath 'capture_lifecycle_test.c'
$captureStubs = Join-Path $testPath 'capture_host_stubs'
$audioMetrics = Join-Path $repository 'firmware\components\audio_metrics\include'
$vad = Join-Path $repository 'firmware\components\vad\include'
# Only compiler invocation uses cmd; filesystem operations stay in PowerShell.
$compile = 'call "' + $compilerSetup + '" >nul && cl /nologo /std:c11 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I"' + $stubs + '" /I"' + $common + '" /I"' + $ring + '" /I"' + $wav + '" "' + $source + '" /Fe:audio_artifact_test.exe'
$compileRecorder = 'call "' + $compilerSetup + '" >nul && cl /nologo /std:c11 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I"' + $recorderStubs + '" /I"' + $common + '" /I"' + $wav + '" /I"' + $recorderPrivate + '" "' + $recorderSource + '" /Fe:recorder_init_test.exe'
<<<<<<< HEAD
$compileRecorderQueue = 'call "' + $compilerSetup + '" >nul && cl /nologo /std:c11 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I"' + $recorderStubs + '" /I"' + $common + '" /I"' + $wav + '" /I"' + $recorderPrivate + '" "' + $recorderQueueSource + '" /Fe:recorder_queue_test.exe'
=======
>>>>>>> fad0770f9a6e5c0128515521f1e160a23e27bf04
$compileAfe = 'call "' + $compilerSetup + '" >nul && cl /nologo /std:c11 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I"' + $afeStubs + '" /I"' + $common + '" /I"' + $dsp + '" /I"' + $dspPrivate + '" "' + $afeSource + '" /Fe:afe_init_test.exe'
$compilePipeline = 'call "' + $compilerSetup + '" >nul && cl /nologo /std:c11 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I"' + $pipelineStubs + '" /I"' + $common + '" /I"' + $pipeline + '" /I"' + $capture + '" /I"' + $dsp + '" /I"' + $audioMetrics + '" /I"' + $ring + '" /I"' + $wav + '" /I"' + $vad + '" "' + $pipelineSource + '" /Fe:pipeline_init_test.exe'
$compileCapture = 'call "' + $compilerSetup + '" >nul && cl /nologo /std:c11 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I"' + $captureStubs + '" /I"' + $common + '" /I"' + $capture + '" "' + $captureSource + '" /Fe:capture_lifecycle_test.exe'
$exportStubs = Join-Path $testPath 'export_host_stubs'
$exportSource = Join-Path $testPath 'export_coordination_test.cpp'
$protocolInclude = Join-Path $repository 'firmware\components\application_protocol\include'
$compileExport = 'call "' + $compilerSetup + '" >nul && cl /nologo /std:c++17 /W4 /WX /EHsc /I"' + $exportStubs + '" /I"' + $protocolInclude + '" "' + $exportSource + '" /Fe:export_coordination_test.exe'
Push-Location $build
try {
    & cmd.exe /d /s /c $compile
    if ($LASTEXITCODE -ne 0) { throw "Audio artifact test compilation failed: $LASTEXITCODE" }
    & '.\audio_artifact_test.exe'
    if ($LASTEXITCODE -ne 0) { throw "Audio artifact tests failed: $LASTEXITCODE" }
    & cmd.exe /d /s /c $compileRecorder
    if ($LASTEXITCODE -ne 0) { throw "Recorder lifecycle test compilation failed: $LASTEXITCODE" }
    & '.\recorder_init_test.exe'
    if ($LASTEXITCODE -ne 0) { throw "Recorder lifecycle tests failed: $LASTEXITCODE" }
<<<<<<< HEAD
    & cmd.exe /d /s /c $compileRecorderQueue
    if ($LASTEXITCODE -ne 0) { throw "Recorder queue test compilation failed: $LASTEXITCODE" }
    & '.\recorder_queue_test.exe'
    if ($LASTEXITCODE -ne 0) { throw "Recorder queue tests failed: $LASTEXITCODE" }
=======
>>>>>>> fad0770f9a6e5c0128515521f1e160a23e27bf04
    & cmd.exe /d /s /c $compileAfe
    if ($LASTEXITCODE -ne 0) { throw "AFE lifecycle test compilation failed: $LASTEXITCODE" }
    & '.\afe_init_test.exe'
    if ($LASTEXITCODE -ne 0) { throw "AFE lifecycle tests failed: $LASTEXITCODE" }
    & cmd.exe /d /s /c $compilePipeline
    if ($LASTEXITCODE -ne 0) { throw "Pipeline startup test compilation failed: $LASTEXITCODE" }
    & '.\pipeline_init_test.exe'
    if ($LASTEXITCODE -ne 0) { throw "Pipeline startup tests failed: $LASTEXITCODE" }
    & cmd.exe /d /s /c $compileCapture
    if ($LASTEXITCODE -ne 0) { throw "Capture lifecycle test compilation failed: $LASTEXITCODE" }
    & '.\capture_lifecycle_test.exe'
    if ($LASTEXITCODE -ne 0) { throw "Capture lifecycle tests failed: $LASTEXITCODE" }
    & cmd.exe /d /s /c $compileExport
    if ($LASTEXITCODE -ne 0) { throw "Export coordination test compilation failed: $LASTEXITCODE" }
    & '.\export_coordination_test.exe'
    if ($LASTEXITCODE -ne 0) { throw "Export coordination tests failed: $LASTEXITCODE" }
} finally { Pop-Location }
