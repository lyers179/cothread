$env:Path = "C:\mingw64\mingw64\bin;C:\mingw64\bin;C:\Windows\System32;" + $env:Path
Set-Location "G:\bthread\build_ninja"
& ".\simple_mutex_test.exe"
Write-Host "Exit code: $LASTEXITCODE"