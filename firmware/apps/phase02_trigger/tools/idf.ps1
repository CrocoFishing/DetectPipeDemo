param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
& "$PSScriptRoot\..\..\..\..\tools\idf.ps1" -App phase02_trigger @IdfArgs
exit $LASTEXITCODE

