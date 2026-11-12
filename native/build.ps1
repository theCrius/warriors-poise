# Builds warriors-poise-native.dll with the MSVC Build Tools and puts it into the mod package
# (mods/warriors-poise/bin/), where native-plugin.ini points.
# Usage: ./native/build.ps1 [-Config Release|Debug]
param([ValidateSet('Release', 'Debug')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'

# Pinned compiler: the same source built by another MSVC version gives a different DLL, and only a
# DLL someone has run in game should ship. GitHub's runner and every local build use exactly this
# toolset; a machine without it gets an error, never a silent build with another compiler.
$VsVersions = '[17.14,18.0)'  # Visual Studio 2022 17.14 (any edition, Build Tools included)
$Toolset = '14.44.35207'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Installer not found: install Visual Studio 2022 Build Tools 17.14 (see README, Building from source)' }
$vs = & $vswhere -products * -version $VsVersions -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath
if (-not $vs) { throw "Visual Studio 2022 17.14 with the C++ x64/x86 build tools not found (see README, Building from source)" }
if (-not (Test-Path (Join-Path $vs "VC\Tools\MSVC\$Toolset"))) {
    throw "MSVC toolset $Toolset not found in $vs. In the Visual Studio Installer, add the individual component " +
          "'MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)' (see README, Building from source)"
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat calls vswhere by name

$src = Join-Path $PSScriptRoot 'warriors-poise-native'
$obj = Join-Path $PSScriptRoot "build\$Config"
$bin = Join-Path $PSScriptRoot '..\mods\warriors-poise\bin'
New-Item -ItemType Directory -Force -Path $obj, $bin | Out-Null
$dll = Join-Path $obj 'warriors-poise-native.dll'
$log = Join-Path $obj 'build.log'

# /MT: static CRT, so the DLL needs no Visual C++ runtime on the player's machine.
$opt = if ($Config -eq 'Release') { '/O2 /DNDEBUG' } else { '/Od /Zi' }
# /Brepro: no build timestamps, so the same source gives a byte-identical DLL.
$cl = "cl /nologo /std:c++17 /W4 /WX /EHsc /MT /Brepro $opt /DUNICODE /D_UNICODE /LD " +
      "`"$src\dllmain.cpp`" /Fo`"$obj\\`" /Fd`"$obj\\`" /Fe`"$dll`" /link /DLL /INCREMENTAL:NO /Brepro kernel32.lib"
cmd /c "call `"$vcvars`" -vcvars_ver=$Toolset >nul && ($cl) > `"$log`" 2>&1"
if ($LASTEXITCODE -ne 0) { Get-Content $log | Select-String -Pattern 'error|warning' | Select-Object -First 30; throw "build failed, see $log" }

# The linker writes its version into the PE header: check that the pin held.
$bytes = [IO.File]::ReadAllBytes($dll)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$linker = '{0}.{1}' -f $bytes[$pe + 26], $bytes[$pe + 27]
if (-not $Toolset.StartsWith("$linker.")) { throw "linked by MSVC $linker, expected $Toolset" }

Copy-Item $dll $bin -Force
$out = Get-Item (Join-Path $bin 'warriors-poise-native.dll')
'{0}  {1:N0} bytes  SHA-256 {2}  (MSVC {3})' -f $out.FullName, $out.Length, (Get-FileHash $out.FullName).Hash, $Toolset
