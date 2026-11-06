# Builds clash-and-cleave-native.dll with the MSVC Build Tools and puts it into the mod package
# (mods/clash-and-cleave/bin/), where native-plugin.ini points.
# Usage: ./native/build.ps1 [-Config Release|Debug]
param([ValidateSet('Release', 'Debug')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC Build Tools not found (vswhere)' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat calls vswhere by name

$src = Join-Path $PSScriptRoot 'clash-and-cleave-native'
$obj = Join-Path $PSScriptRoot "build\$Config"
$bin = Join-Path $PSScriptRoot '..\mods\clash-and-cleave\bin'
New-Item -ItemType Directory -Force -Path $obj, $bin | Out-Null
$dll = Join-Path $obj 'clash-and-cleave-native.dll'
$log = Join-Path $obj 'build.log'

# /MT: static CRT, so the DLL needs no Visual C++ runtime on the player's machine.
$opt = if ($Config -eq 'Release') { '/O2 /DNDEBUG' } else { '/Od /Zi' }
# /Brepro: no build timestamps, so the same source gives a byte-identical DLL.
$cl = "cl /nologo /std:c++17 /W4 /WX /EHsc /MT /Brepro $opt /DUNICODE /D_UNICODE /LD " +
      "`"$src\dllmain.cpp`" /Fo`"$obj\\`" /Fd`"$obj\\`" /Fe`"$dll`" /link /DLL /INCREMENTAL:NO /Brepro kernel32.lib"
cmd /c "call `"$vcvars`" >nul && ($cl) > `"$log`" 2>&1"
if ($LASTEXITCODE -ne 0) { Get-Content $log | Select-String -Pattern 'error|warning' | Select-Object -First 30; throw "build failed, see $log" }

Copy-Item $dll $bin -Force
$out = Get-Item (Join-Path $bin 'clash-and-cleave-native.dll')
'{0}  {1:N0} bytes  SHA-256 {2}' -f $out.FullName, $out.Length, (Get-FileHash $out.FullName).Hash
