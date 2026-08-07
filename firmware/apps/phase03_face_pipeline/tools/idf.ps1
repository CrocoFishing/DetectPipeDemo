param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
& "$PSScriptRoot\..\..\..\..\tools\idf.ps1" -App phase03_face_pipeline @IdfArgs
exit $LASTEXITCODE

