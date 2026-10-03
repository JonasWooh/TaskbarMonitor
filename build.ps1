# Builds bin\TaskbarMonitor.exe with LLVM-MinGW (winget install MartinStorsjo.LLVM-MinGW.UCRT).
param([string]$Out = 'bin/TaskbarMonitor.exe')
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force (Split-Path $Out), obj | Out-Null

windres -i res/app.rc -o obj/app.res.o -O coff --include-dir res
if ($LASTEXITCODE) { exit $LASTEXITCODE }

clang++ -std=c++17 -O2 -municode -mwindows -fno-exceptions -fno-rtti -fno-threadsafe-statics `
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -DNOMINMAX -Wall `
    src/main.cpp src/metrics.cpp src/pawnio.cpp src/format.cpp src/panel.cpp src/netproc.cpp src/tbspace.cpp src/lang.cpp `
    obj/app.res.o `
    -static -s '-Wl,--gc-sections' `
    -lpdh -liphlpapi -lgdi32 -luser32 -lshell32 -ladvapi32 -lole32 -loleaut32 -ldwmapi `
    -o $Out
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Get-Item $Out | Select-Object Name, Length

