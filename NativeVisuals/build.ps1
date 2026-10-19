$ErrorActionPreference='Stop'
$taskSource=$PSScriptRoot
$taskBuild=Join-Path (Split-Path -Parent $taskSource) 'artifacts/visuals-build'
$taskVsWhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs=& $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$taskVs) { throw 'Install Visual Studio Desktop development with C++ and CMake tools first.' }
& (Join-Path $taskVs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
# Keep compiler diagnostics and CMake's /showIncludes prefix in the same encoding.
# A mismatched prefix silently leaves changed headers (including shaders) out of incremental builds.
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
chcp 65001 | Out-Null
cmake -S $taskSource -B $taskBuild -G Ninja -DCMAKE_BUILD_TYPE=Release
if($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $taskBuild
exit $LASTEXITCODE
