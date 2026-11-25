<#
.SYNOPSIS
Builds SeamlessSkyrim; copies it into the game when a plugins folder was configured.

.DESCRIPTION
The one way to build from a shell: cmake isn't on PATH on this machine (it comes with Visual
Studio), and the post-build copy into the game, when one is configured (CMake's
SKYRIM_PLUGINS_DIR, empty by default), fails while the game runs, with an error that reads like a
build failure.

Prints compiler warnings and errors only, then one line saying what was built and whether it
reached the game.

Runs on Windows PowerShell 5.1 as well.

.EXAMPLE
tools\build.ps1                      # RelWithDebInfo, copied into SKYRIM_PLUGINS_DIR if set
.EXAMPLE
tools\build.ps1 -NoDeploy            # does it compile? Safe while the game runs
#>
[CmdletBinding()]
param(
	# RelWithDebInfo is the day-to-day build: its PDB gives Crash Logger function names and lines.
	# Release is what the release archive ships.
	[ValidateSet('RelWithDebInfo', 'Release', 'Debug')][string]$Config = 'RelWithDebInfo',
	# Compile and link only; the game's files stay as they are.
	[switch]$NoDeploy
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

if (-not $NoDeploy -and (Get-Process SkyrimSE -ErrorAction SilentlyContinue)) {
	throw 'Skyrim is running and holds the DLL: quit it first (never a game the user is playing), or pass -NoDeploy to only compile.'
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
if (-not $cmake) {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (Test-Path -LiteralPath $vswhere) {
		$cmake = & $vswhere -latest -prerelease -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
	}
}
if (-not $cmake) { throw 'cmake not found (neither on PATH nor in Visual Studio).' }

$msbuild = @('-nologo')
# The copy into the game is the project's post-build event; this turns it off for one build
# without reconfiguring.
$noCopy = @('-p:PostBuildEventUseInBuild=false')

$pluginArgs = $msbuild
if ($NoDeploy) { $pluginArgs += $noCopy }
& $cmake --build (Join-Path $repo 'build') --config $Config -- @pluginArgs
if ($LASTEXITCODE) { throw 'SeamlessSkyrim build failed.' }
$dll = Join-Path $repo "build\$Config\SeamlessSkyrim.dll"
$where = if ($NoDeploy) { 'not copied into the game' } else { 'copied into the game if SKYRIM_PLUGINS_DIR is set' }
Write-Output "SeamlessSkyrim ($Config) built: $dll, $where."
