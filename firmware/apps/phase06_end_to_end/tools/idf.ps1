param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
& "$PSScriptRoot\..\..\..\..\tools\idf.ps1" -App phase06_end_to_end @IdfArgs
exit $LASTEXITCODE
