<#
.SYNOPSIS
Moves the doors tuned in game into package\'s SeamlessSkyrim_Doors.ini, the doors the mod ships with.

.DESCRIPTION
The plugin writes in-game tunings to the game's SeamlessSkyrim_Doors_User.ini, never to
SeamlessSkyrim_Doors.ini: a player's update replaces the shipped file, and their tunings with it
if they lived there. The tunings made in game are the mod's defaults to be, so this script
moves them into package\:

- A door in the user file replaces all of its entries in package\'s file, as the plugin reads
  them. [DoorPortal]=1, which the plugin writes only to keep a tuned door in the user file, is
  dropped: on is the default.
- The result is written into the game's SeamlessSkyrim_Doors.ini as well, and the user file is
  deleted: the game reads the same doors as before.

Nothing happens without a user file. Commit package\'s file afterwards.
#>
#Requires -Version 7
[CmdletBinding()]
param(
	# The Data folder whose SeamlessSkyrim_Doors_User.ini holds the tunings.
	[Parameter(Mandatory = $true)][string]$GameData
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$plugins = 'SKSE\Plugins'
$doors = Join-Path $repo "package\$plugins\SeamlessSkyrim_Doors.ini"
$liveDoors = Join-Path $GameData "$plugins\SeamlessSkyrim_Doors.ini"
$tunings = Join-Path $GameData "$plugins\SeamlessSkyrim_Doors_User.ini"
if (-not (Test-Path -LiteralPath $tunings)) { return }

# The plugin's writer (WritePrivateProfileString) leaves the ANSI code page; Latin1 passes every
# byte through unchanged.
$encoding = [Text.Encoding]::Latin1

# Section name -> list of "Plugin.esp|ID=value" lines, in file order. Keys compare without case,
# as the plugin's reader does.
function Read-Sections([string]$Path) {
	$sections = [ordered]@{}
	$current = $null
	foreach ($line in [IO.File]::ReadAllLines($Path, $encoding)) {
		$line = $line.Trim()
		if (-not $line -or $line.StartsWith(';')) { continue }
		if ($line -match '^\[(.+)\]$') {
			$current = $Matches[1]
			if (-not $sections.Contains($current)) { $sections[$current] = [Collections.Generic.List[string]]::new() }
		}
		elseif ($current) { $sections[$current].Add($line) }
	}
	$sections
}
function Get-Door([string]$Entry) { ($Entry -split '=', 2)[0].ToLowerInvariant() }

$shipped = Read-Sections $doors
$tuned = Read-Sections $tunings
$tunedDoors = [Collections.Generic.HashSet[string]]::new()
foreach ($entries in $tuned.Values) { foreach ($entry in $entries) { [void]$tunedDoors.Add((Get-Door $entry)) } }

foreach ($section in @($shipped.Keys)) {
	$shipped[$section] = [Collections.Generic.List[string]]@($shipped[$section] | Where-Object { -not $tunedDoors.Contains((Get-Door $_)) })
}
foreach ($section in $tuned.Keys) {
	# [ordered] compares keys without case, like the plugin's reader.
	if (-not $shipped.Contains($section)) { $shipped[$section] = [Collections.Generic.List[string]]::new() }
	foreach ($entry in $tuned[$section]) {
		if ($section -eq 'DoorPortal' -and $entry -match '=\s*1\s*$') { continue }
		$shipped[$section].Add($entry)
	}
}

$text = [Text.StringBuilder]::new()
foreach ($section in $shipped.Keys) {
	[void]$text.Append("[$section]`r`n")
	foreach ($entry in $shipped[$section]) { [void]$text.Append("$entry`r`n") }
}
[IO.File]::WriteAllText($doors, $text.ToString(), $encoding)
if (Test-Path -LiteralPath $liveDoors) { Copy-Item -LiteralPath $doors -Destination $liveDoors -Force }
Remove-Item -LiteralPath $tunings
Write-Host "$($tunedDoors.Count) door tuning(s) from the game moved into package\$plugins\SeamlessSkyrim_Doors.ini; commit them." -ForegroundColor Yellow
