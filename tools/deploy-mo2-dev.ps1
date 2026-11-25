<#
.SYNOPSIS
Puts a SeamlessSkyrim build into the Skyrim modding project's MO2 instance as a local mod.

.DESCRIPTION
The mod folder holds only SKSE\Plugins\SeamlessSkyrim.dll (and .pdb). The 'Seamless Dev Build'
entry sits right after 'Seamless Skyrim' in the project's modlist.yaml, so in MO2 it wins over the
Nexus release's DLL, while that release's Community Shaders files, ini files and shaders stay.

Two copies are written, with the source's modification time, so the project's build.py (which
mirrors .weaver\files\<mod> into instance\mods\<mod> and skips files with equal size and time)
leaves them alone:
  <project>\.weaver\files\<ModName>\SKSE\Plugins   what modlist.yaml's local: entry mirrors
  <project>\instance\mods\<ModName>\SKSE\Plugins   what MO2 uses (when the mod exists in the instance)
Nothing needs a rebuild of the instance after the first one.

Refuses to run while SkyrimSE.exe is running: a running game keeps its DLL locked, and it may be
the user playing.

.EXAMPLE
tools\build.ps1 -NoDeploy; tools\deploy-mo2-dev.ps1          # the build tree's RelWithDebInfo
tools\deploy-mo2-dev.ps1 -Baseline                           # .weaver\baseline (upstream, for comparison)
#>
param(
	# Deploy .weaver\baseline's DLL and PDB (the upstream plugin) instead of build\RelWithDebInfo's.
	[switch]$Baseline,
	# A folder holding SeamlessSkyrim.dll and .pdb, instead of either.
	[string]$Source,
	[switch]$NoPdb,
	[string]$SkyrimProject = 'D:\AI\Active\Skyrim',
	[string]$ModName = 'Seamless Dev Build'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) {
	throw 'SkyrimSE.exe is running (the user playing, or another test): not deploying. Quit your own game first.'
}

if (-not $Source) {
	$Source = if ($Baseline) { Join-Path $repo '.weaver\baseline' } else { Join-Path $repo 'build\RelWithDebInfo' }
}
$files = @('SeamlessSkyrim.dll')
if (-not $NoPdb) { $files += 'SeamlessSkyrim.pdb' }
foreach ($file in $files) {
	if (-not (Test-Path (Join-Path $Source $file))) { throw "$Source\$file not found (tools\build.ps1 -NoDeploy builds it)" }
}

$targets = @(Join-Path $SkyrimProject ".weaver\files\$ModName\SKSE\Plugins")
$instanceMod = Join-Path $SkyrimProject "instance\mods\$ModName"
if (Test-Path $instanceMod) {
	$targets += Join-Path $instanceMod 'SKSE\Plugins'
} else {
	Write-Host "instance\mods\$ModName does not exist yet: run 'uv run .weaver/tools/build.py --only `"$ModName`"' from the PowerShell tool in $SkyrimProject once (needs the local: entry in modlist.yaml)."
}

foreach ($target in $targets) {
	New-Item -ItemType Directory -Force $target | Out-Null
	foreach ($file in $files) {
		Copy-Item -Force (Join-Path $Source $file) (Join-Path $target $file)
	}
	if ($NoPdb) { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $target 'SeamlessSkyrim.pdb') }
	Write-Host "deployed $($files -join ', ') from $Source to $target"
}
$dll = Get-Item (Join-Path $Source 'SeamlessSkyrim.dll')
Write-Host ("DLL: {0:N0} bytes, built {1}" -f $dll.Length, $dll.LastWriteTime)
