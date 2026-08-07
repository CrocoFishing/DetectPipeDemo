param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
& "$PSScriptRoot\..\..\..\..\tools\idf.ps1" -App phase05_ble @IdfArgs
exit $LASTEXITCODE

