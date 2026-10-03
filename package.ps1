# Builds the release into release\: TaskbarMonitor.exe (one self-contained exe:
# static runtime, icon and PawnIO module embedded) and a zip with the readmes and
# license files, ready to attach to a GitHub release.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
& .\build.ps1 -Out release/TaskbarMonitor.exe | Out-Null
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$version = (Get-Item release/TaskbarMonitor.exe).VersionInfo.ProductVersion
$stage = Join-Path obj "TaskbarMonitor-$version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null
Copy-Item release/TaskbarMonitor.exe, README.md, README.zh-CN.md, LICENSE $stage
Copy-Item third_party/pawnio-modules/COPYING (Join-Path $stage 'PawnIO.Modules-LICENSE.txt')
Get-ChildItem release -Filter *.zip | Remove-Item
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath "release/TaskbarMonitor-$version-win-x64.zip"

Get-ChildItem release | Select-Object Name, Length
