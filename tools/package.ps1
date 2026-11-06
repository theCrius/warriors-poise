# Builds the native DLL (Release) and packages a release zip:
#   dist/warriors-poise-<version>.zip (plus the same file as dist/warriors-poise.zip)
#     warriors-poise/            the ShroudForge mod folder (copy into <game>/mods/)
#     README.md, LICENSE, NOTICE
# The version comes from mods/warriors-poise/mod.json. When run for a git tag (CI sets
# GITHUB_REF_TYPE=tag), the tag must be v<version>.
# Usage: ./tools/package.ps1
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$mod = Join-Path $root 'mods\warriors-poise'
$version = (Get-Content -Raw (Join-Path $mod 'mod.json') | ConvertFrom-Json).version
if ($env:GITHUB_REF_TYPE -eq 'tag' -and $env:GITHUB_REF_NAME -ne "v$version") {
    throw "tag $($env:GITHUB_REF_NAME) does not match mod.json version $version"
}

# A clean bin: anything left there from an older build would end up in the zip.
$bin = Join-Path $mod 'bin'
if (Test-Path $bin) { Remove-Item -Recurse -Force $bin }
& (Join-Path $root 'native\build.ps1') -Config Release

$dist = Join-Path $root 'dist'
$stage = Join-Path $dist "warriors-poise-$version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Mod folder: package files only.
$modOut = Join-Path $stage 'warriors-poise'
New-Item -ItemType Directory -Force -Path $modOut | Out-Null
foreach ($item in 'mod.json', 'extended.mod.json', 'native-plugin.ini', 'src', 'bin') {
    Copy-Item -Recurse (Join-Path $mod $item) $modOut
}

Copy-Item (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE'), (Join-Path $root 'NOTICE') $stage

$zip = Join-Path $dist "warriors-poise-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
'{0}  {1:N0} bytes  SHA-256 {2}' -f $zip, (Get-Item $zip).Length, (Get-FileHash $zip).Hash

# Same zip under a name without the version, so .../releases/latest/download/warriors-poise.zip
# always points at the newest release.
$latest = Join-Path $dist 'warriors-poise.zip'
Copy-Item $zip $latest -Force
'{0}  (copy for the latest-release link)' -f $latest
