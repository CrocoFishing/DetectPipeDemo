$python = "$PSScriptRoot\..\..\pc_client\.venv\Scripts\python.exe"
& $python -m pytest "$PSScriptRoot\..\..\pc_client\tests\test_protocol.py" "$PSScriptRoot\..\..\pc_client\tests\test_protocol_consistency.py" -q
exit $LASTEXITCODE

