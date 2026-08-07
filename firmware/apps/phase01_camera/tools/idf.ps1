param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
& "$PSScriptRoot\..\..\..\..\tools\idf.ps1" -App phase01_camera @IdfArgs
exit $LASTEXITCODE

